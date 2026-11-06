// SPDX-License-Identifier: MIT
// ── PDF 阅读 App：列表 / 阅读视图 / 浮动设置 / 触摸 ────────────────────────
// 文档解析与页缓存全在 screen_reader_pdf_cache.cpp；这里只做界面与输入。
// 书身份 bookKind=5，模式恒为 RdMode::Pdf（不进 Reading 的排版/字体/目录管线）。
#include <dirent.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <hw/input.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "bt_keyboard.h"
#include "screen_reader_internal.h"
#include "settings_manager.h"

static const char *TAG = "Pdf";

// ── 设置键（沿用 SettingsManager 的"一键一文件"，旧固件不认识这些键也无害）──
static const char *kKeyZoomMode = "pdf_zoom_mode";
static const char *kKeyCropMode = "pdf_crop_mode";
static const char *kKeyRotate = "pdf_rotate";

static const int64_t kDoubleTapUs = 300 * 1000;
static const int kDoubleTapPx = 40;

static void pdfLoadPrefs() {
  const int zm = atoi(g_settings.getString(kKeyZoomMode, "1").c_str());
  const int cm = atoi(g_settings.getString(kKeyCropMode, "0").c_str());
  const int rot = atoi(g_settings.getString(kKeyRotate, "0").c_str());
  st.pdf.zoomMode = (zm >= 0 && zm <= 2) ? zm : 1;
  st.pdf.cropMode = (cm == 1) ? 1 : 0;
  st.pdf.rotate = (rot == 90 || rot == 180 || rot == 270) ? rot : 0;
}

static void pdfSavePrefs() {
  g_settings.setString(kKeyZoomMode, std::to_string(st.pdf.zoomMode));
  g_settings.setString(kKeyCropMode, std::to_string(st.pdf.cropMode));
  g_settings.setString(kKeyRotate, std::to_string(st.pdf.rotate));
}

static std::string pdfBaseName(const std::string &p) {
  const size_t s = p.find_last_of('/');
  return (s == std::string::npos) ? p : p.substr(s + 1);
}

static bool pdfIsPdfName(const std::string &n) {
  return endsWith(n, ".pdf") || endsWith(n, ".PDF");
}

// ── 列表 ────────────────────────────────────────────────────────────────
// 扫两个位置：/sdcard 根与 /sdcard/books（存在时）。不递归 —— 大卡上递归扫目录
// 会把打开动作拖成秒级。文件管理器里点 .pdf 仍可直接读。
static void pdfScanList() {
  st.pdf.list.clear();
  const char *dirs[] = {"/sdcard", "/sdcard/books"};
  for (const char *dir : dirs) {
    DIR *dp = opendir(dir);
    if (!dp) continue;
    struct dirent *e = nullptr;
    while ((e = readdir(dp)) != nullptr) {
      if (e->d_name[0] == '.') continue;
      if (e->d_type == DT_DIR) continue;
      if (!pdfIsPdfName(e->d_name)) continue;
      PdfListItem it;
      it.path = std::string(dir) + "/" + e->d_name;
      it.name = e->d_name;
      st.pdf.list.push_back(std::move(it));
    }
    closedir(dp);
  }
  std::sort(st.pdf.list.begin(), st.pdf.list.end(),
            [](const PdfListItem &a, const PdfListItem &b) { return a.name < b.name; });
  if (st.pdf.listSel >= static_cast<int>(st.pdf.list.size())) {
    st.pdf.listSel = static_cast<int>(st.pdf.list.size()) - 1;
  }
  if (st.pdf.listSel < 0) st.pdf.listSel = 0;
}

static ListView pdfListGeometry() {
  ListView lv;
  lv.top = coverTop();
  lv.itemH = uiLineHeight() + 8;
  lv.count = static_cast<int>(st.pdf.list.size());
  lv.rows = std::max(1, (statusTop() - lv.top) / lv.itemH);
  lv.sel = st.pdf.listSel;
  listViewCenter(lv);
  return lv;
}

// ── 浮动设置菜单（PDF 专用；阅读模式的 sheetVisible 钉死在 Reading，不硬塞）──
struct PdfMenuRow {
  const char *label;
  const char *const *names;
  int count;
  int *value;
  const char *key;
};

static const char *const kZoomNames[] = {"适合宽度", "自适应", "原始大小"};
static const char *const kCropNames[] = {"不裁边", "自动裁边"};
static const char *const kRotateNames[] = {"不旋转", "逆时针90°", "顺时针90°", "旋转180°"};
static const int kRotateValues[] = {0, 270, 90, 180};

static void pdfJumpOpen();
static bool pdfLoadPage(int page, bool forceFull);

static int pdfMenuRowCount() { return 4; }
static int pdfMenuRowH() { return uiLineHeight() + 12; }
static int pdfMenuH() { return pdfMenuRowCount() * pdfMenuRowH() + 12; }
static int pdfMenuTop() { return std::max(0, statusTop() - pdfMenuH() - 4); }
static int pdfMenuX() { return MARGIN; }
static int pdfMenuW() { return g_rd.getScreenWidth() - 2 * MARGIN; }

static const char *pdfMenuLabel(int row) {
  switch (row) {
    case 0: return "缩放方式";
    case 1: return "裁边方式";
    case 2: return "图像旋转";
    default: return "跳转到页";
  }
}

static int pdfMenuValue(int row) {
  if (row == 0) return st.pdf.zoomMode;
  if (row == 1) return st.pdf.cropMode;
  if (row == 2) {
    for (int i = 0; i < 4; i++) {
      if (kRotateValues[i] == st.pdf.rotate) return i;
    }
    return 0;
  }
  return st.pdf.page + 1;   // 跳转行显示当前页码
}

static int pdfMenuValueCount(int row) {
  return row == 0 ? 3 : (row == 1 ? 2 : (row == 2 ? 4 : 1));
}

static void pdfMenuSetValue(int row, int v) {
  if (row == 0) st.pdf.zoomMode = v;
  else if (row == 1) st.pdf.cropMode = v;
  else if (row == 2) st.pdf.rotate = kRotateValues[v];
}

static void pdfMenuCycle(int row, int dir) {
  if (row == 3) {   // 跳转行：不是循环取值，而是弹数字输入框
    pdfJumpOpen();
    return;
  }
  const int n = pdfMenuValueCount(row);
  int v = (pdfMenuValue(row) + dir) % n;
  if (v < 0) v += n;
  pdfMenuSetValue(row, v);
  pdfSavePrefs();
  // 改缩放方式/裁边/旋转都直接影响画面：整屏重画 + 下次刷新由 PDF 的差分/全刷分支接住。
  st.pdf.zoom = 1.0f;
  st.pdf.panX = st.pdf.panY = 0.5f;
  st.pdf.diffSteps = 0;
  st.fullRefresh = true;   // 旋转/裁边/缩放方式都换了一张图，GC16 清场最稳
  st.dirty = 1;
}

static void pdfDrawMenu() {
  const int x = pdfMenuX(), w = pdfMenuW();
  const int h = pdfMenuH(), top = pdfMenuTop();
  g_rd.fillRect(x, top, w, h, false);
  g_rd.drawRect(x, top, w, h, true);
  const int lh = uiLineHeight();
  for (int r = 0; r < pdfMenuRowCount(); r++) {
    const int y = top + 6 + r * pdfMenuRowH();
    const bool sel = (r == st.pdf.menuSel);
    if (sel) g_rd.fillRect(x + 4, y, w - 8, pdfMenuRowH() - 2, true);
    const char *label = pdfMenuLabel(r);
    int idx = pdfMenuValue(r);
    char pageBuf[24];
    const char *val;
    if (r == 0) {
      val = kZoomNames[idx];
    } else if (r == 1) {
      val = kCropNames[idx];
    } else if (r == 2) {
      val = kRotateNames[idx];
    } else {
      snprintf(pageBuf, sizeof(pageBuf), "第 %d 页", st.pdf.page + 1);
      val = pageBuf;
    }
    drawLineText(x + 12, y + (pdfMenuRowH() - lh) / 2, label, !sel);
    const int vw = g_rd.getTextWidth(uiFontId(), val);
    drawLineText(x + w - 12 - vw, y + (pdfMenuRowH() - lh) / 2, val, !sel);
  }
}

static bool pdfMenuHit(int x, int y, int *row) {
  const int mx = pdfMenuX(), mw = pdfMenuW(), mt = pdfMenuTop();
  if (x < mx || x >= mx + mw || y < mt || y >= mt + pdfMenuH()) return false;
  if (!row) return true;
  const int r = (y - mt - 6) / pdfMenuRowH();
  *row = (r >= 0 && r < pdfMenuRowCount()) ? r : -1;
  return true;
}

// ── 跳转到页：数字输入框（菜单第 4 行弹出）──────────────────────────────
// 自绘数字键盘：触屏点键、实体数字键、退格、回车/确定都能用；Esc 取消。
struct PdfJumpLayout {
  int x, y, w, h;         // 卡片
  int gx, gy, keyW, keyH, gap;
  int titleY, inputY;
};

static PdfJumpLayout pdfJumpLayout() {
  PdfJumpLayout L;
  const int sw = g_rd.getScreenWidth();
  const int lh = uiLineHeight();
  L.gap = 8;
  L.w = std::min(sw - 2 * MARGIN, 640);
  L.keyW = (L.w - 2 * 12 - 2 * L.gap) / 3;
  L.keyH = lh + 24;
  L.h = 12 + lh + 8 + lh + 12 + 4 * L.keyH + 3 * L.gap + 12;
  L.x = (sw - L.w) / 2;
  L.y = std::max(MARGIN, (statusTop() - L.h) / 2);
  L.titleY = L.y + 12;
  L.inputY = L.titleY + lh + 8;
  L.gx = L.x + 12;
  L.gy = L.inputY + lh + 12;
  return L;
}

static void pdfJumpKeyRect(const PdfJumpLayout &L, int i, int *x, int *y) {
  *x = L.gx + (i % 3) * (L.keyW + L.gap);
  *y = L.gy + (i / 3) * (L.keyH + L.gap);
}

static const char *const kJumpKeyLabels[12] = {"1", "2", "3", "4", "5", "6",
                                               "7", "8", "9", "退格", "0", "确定"};

static void pdfDrawJump() {
  const PdfJumpLayout L = pdfJumpLayout();
  const int lh = uiLineHeight();
  g_rd.fillRect(L.x, L.y, L.w, L.h, false);
  g_rd.drawRect(L.x, L.y, L.w, L.h, true);
  drawLineText(L.x + 12, L.titleY, "跳转到页", true);
  char buf[64];
  const int pc = std::max(1, st.pdf.pageCount);
  if (st.pdf.jumpLen == 0) {
    snprintf(buf, sizeof(buf), "第 ___ 页 / 共 %d 页", pc);
  } else {
    snprintf(buf, sizeof(buf), "第 %s 页 / 共 %d 页", st.pdf.jumpDigits, pc);
  }
  drawLineText(L.x + 12, L.inputY, buf, true);
  for (int i = 0; i < 12; i++) {
    int kx = 0, ky = 0;
    pdfJumpKeyRect(L, i, &kx, &ky);
    g_rd.fillRect(kx, ky, L.keyW, L.keyH, false);
    g_rd.drawRect(kx, ky, L.keyW, L.keyH, true);
    const char *lab = kJumpKeyLabels[i];
    const int tw = g_rd.getTextWidth(uiFontId(), lab);
    drawLineText(kx + (L.keyW - tw) / 2, ky + (L.keyH - lh) / 2, lab, true);
  }
}

// 返回键序号；-2 = 点在卡片外；-1 = 卡片内但没点在键上。
static int pdfJumpHitKey(int x, int y) {
  const PdfJumpLayout L = pdfJumpLayout();
  if (x < L.x || x >= L.x + L.w || y < L.y || y >= L.y + L.h) return -2;
  for (int i = 0; i < 12; i++) {
    int kx = 0, ky = 0;
    pdfJumpKeyRect(L, i, &kx, &ky);
    if (x >= kx && x < kx + L.keyW && y >= ky && y < ky + L.keyH) return i;
  }
  return -1;
}

static void pdfJumpOpen() {
  st.pdf.jumpOpen = true;
  st.pdf.jumpLen = 0;
  st.pdf.jumpDigits[0] = '\0';
  st.dirty = 1;
}

static void pdfJumpClose() {
  st.pdf.jumpOpen = false;
  st.dirty = 1;
}

static void pdfJumpDigit(int d) {
  if (st.pdf.jumpLen >= 5) return;                    // 页数上限 5000，五位足够
  if (st.pdf.jumpLen == 0 && d == 0) return;          // 不记前导零
  st.pdf.jumpDigits[st.pdf.jumpLen++] = static_cast<char>('0' + d);
  st.pdf.jumpDigits[st.pdf.jumpLen] = '\0';
  st.dirty = 1;
}

static void pdfJumpBackspace() {
  if (st.pdf.jumpLen == 0) return;
  st.pdf.jumpDigits[--st.pdf.jumpLen] = '\0';
  st.dirty = 1;
}

static void pdfJumpConfirm() {
  if (st.pdf.jumpLen == 0) return;
  int v = 0;
  for (int i = 0; i < st.pdf.jumpLen; i++) v = v * 10 + (st.pdf.jumpDigits[i] - '0');
  v = clampI(v, 1, std::max(1, st.pdf.pageCount));
  st.pdf.jumpOpen = false;
  st.pdf.menuOpen = false;
  pdfLoadPage(v - 1, true);
}

static void pdfHandleJumpKey(int key) {
  if (key == 0x1B || key == KEY_LONG_CONFIRM) {
    pdfJumpClose();   // 回到设置菜单
    return;
  }
  if (key >= '0' && key <= '9') {
    pdfJumpDigit(key - '0');
    return;
  }
  if (key == 0x08 || key == 0x7F) {
    pdfJumpBackspace();
    return;
  }
  if (key == '\n' || key == 0x0A || key == 0x0D) {
    int x = 0, y = 0;
    if (!input_tap_xy(&x, &y)) {   // 实体回车 = 确定
      pdfJumpConfirm();
      return;
    }
    const int i = pdfJumpHitKey(x, y);
    if (i == -2) {                 // 点卡片外：取消
      pdfJumpClose();
    } else if (i == 9) {
      pdfJumpBackspace();
    } else if (i == 11) {
      pdfJumpConfirm();
    } else if (i >= 0) {
      pdfJumpDigit(i < 9 ? i + 1 : 0);   // 0..8 → 1..9；10 → 0
    }
    return;
  }
  // 输入框打开时吞掉其他键，避免落到翻页/平移
}

// ── 页脚 ────────────────────────────────────────────────────────────────
static void pdfBuildFooter(char *buf, size_t n) {
  static const char *const zoomNames[] = {"适合宽度", "自适应", "原始大小"};
  const int pct = static_cast<int>(st.pdf.zoom * 100.0f + 0.5f);
  if (st.pdf.zoom > 1.01f) {
    snprintf(buf, n, "%d/%d  %s %d%%  Esc 返回", st.pdf.page + 1,
             std::max(1, st.pdf.pageCount), zoomNames[st.pdf.zoomMode], pct);
  } else {
    snprintf(buf, n, "%d/%d  %s  Esc 返回", st.pdf.page + 1, std::max(1, st.pdf.pageCount),
             zoomNames[st.pdf.zoomMode]);
  }
}

// ── 渲染 ────────────────────────────────────────────────────────────────
static void pdfRenderList() {
  g_rd.clearScreen();
  drawTitle("PDF 阅读");
  const ListView lv = pdfListGeometry();
  if (lv.count == 0) {
    drawCenteredLine(g_rd.getScreenHeight() / 2, "没有找到 PDF");
    drawFooter("把 PDF 放到 /sdcard 或 /sdcard/books  Esc 返回");
    return;
  }
  const int w = g_rd.getScreenWidth();
  for (int i = 0; i < lv.rows && lv.first + i < lv.count; i++) {
    const int idx = lv.first + i;
    const int y = lv.top + i * lv.itemH;
    const std::string label =
        g_rd.truncatedText(uiFontId(), st.pdf.list[idx].name.c_str(), w - 2 * MARGIN);
    if (idx == st.pdf.listSel) {
      g_rd.fillRect(0, y, w, lv.itemH, true);
      drawLineText(MARGIN, y + (lv.itemH - uiLineHeight()) / 2, label.c_str(), false);
    } else {
      drawLineText(MARGIN, y + (lv.itemH - uiLineHeight()) / 2, label.c_str(), true);
    }
  }
  drawFooter("↑↓ 选择  Enter 打开  Esc 返回");
}

static void pdfRenderReader() {
  g_rd.clearScreen();
  const int areaX = 0;
  const int areaY = 0;
  const int areaW = g_rd.getScreenWidth();
  const int areaH = statusTop();
  const int effRot = (st.pdf.rotate % 360 + 360) % 360;
  std::string err;
  if (st.pdf.pageReady &&
      pdfioBlit(st.pdf.page, effRot, st.pdf.zoomMode, st.pdf.zoom, st.pdf.panX, st.pdf.panY,
                areaX, areaY, areaW, areaH, &err)) {
    // 页已经画好
  } else {
    const std::string msg = err.empty() ? (st.pdf.status.empty() ? "正在载入…" : st.pdf.status)
                                        : err;
    drawCenteredLine(g_rd.getScreenHeight() / 2, msg.c_str());
  }
  char foot[96];
  pdfBuildFooter(foot, sizeof(foot));
  drawFooter(foot);
  if (st.pdf.menuOpen) pdfDrawMenu();
  if (st.pdf.jumpOpen) pdfDrawJump();
}

void pdfRender() {
  if (st.pdf.view == PdfView::List) {
    pdfRenderList();
  } else {
    pdfRenderReader();
  }
}

// ── 打开/翻页 ───────────────────────────────────────────────────────────
static bool pdfLoadPage(int page, bool forceFull) {
  const int pc = std::max(1, st.pdf.pageCount);
  page = clampI(page, 0, pc - 1);
  if (page != st.pdf.page) {
    st.pdf.page = page;
    st.pdf.zoom = 1.0f;
    st.pdf.panX = st.pdf.panY = 0.5f;
    st.pdf.panRoomX = st.pdf.panRoomY = 0;   // 下一页的余量等 blit 重新量
    st.pdf.diffSteps = 0;
    st.pdf.prefetchTried = -1;               // 新的下一页重新预解码
  }
  const int effRot = (st.pdf.rotate % 360 + 360) % 360;
  // 先落回"未就绪"再刷载入帧：否则上一页的 pageReady 会让 renderCurrent 在
  // rdShowBusy 里直接把下一页同步解码并画出来，"正在载入"帧永远看不见。
  // 命中缓存的页不推载入帧：那是一次额外的整屏 GC16，翻回看过的页会白闪一下。
  st.pdf.pageReady = false;
  st.pdf.status.clear();
  if (!pdfioPageCached(page, effRot)) {
    rdShowBusy("正在载入…", "第 " + std::to_string(page + 1) + " 页");
  }
  std::string err;
  const bool ok = pdfioEnsurePage(page, effRot, &err);
  st.pdf.pageReady = ok;
  st.pdf.status = ok ? std::string() : err;
  if (forceFull || ok) {
    st.fullRefresh = true;
    st.pdf.diffSteps = 0;
  }
  st.dirty = 1;
  return ok;
}

void pdfEnsureCurrentPage(bool forceFull) {
  st.pdf.view = PdfView::Reader;
  pdfLoadPage(st.pdf.page, forceFull);
}

void pdfEnterList() {
  pdfLoadPrefs();
  st.pdf.view = PdfView::List;
  st.pdf.ret = PdfReturn::Apps;
  st.pdf.menuOpen = false;
  st.pdf.jumpOpen = false;
  st.pdf.tapPending = false;
  st.pdf.status.clear();
  pdfScanList();
  st.tab = 1;
  st.mode = RdMode::Pdf;
  st.fullRefresh = true;
  st.dirty = 1;
}

void pdfOpenFrom(const std::string &path, PdfReturn ret) {
  pdfLoadPrefs();
  st.pdf.ret = ret;
  st.pdf.view = PdfView::Reader;
  st.pdf.menuOpen = false;
  st.pdf.jumpOpen = false;
  st.pdf.tapPending = false;
  st.pdf.page = 0;
  st.pdf.pageCount = 0;
  st.pdf.pageReady = false;
  st.pdf.prefetchTried = -1;
  st.pdf.status.clear();
  st.pdf.zoom = 1.0f;
  st.pdf.panX = st.pdf.panY = 0.5f;
  rdShowBusy("正在打开…", pdfBaseName(path));
  if (!openBook(path, 5)) {
    // 留在阅读视图显示错误（加密/非页面图像型/坏文件），Esc 按来源返回。
    // 清掉书身份：否则帧尾 rdRememberProgress 会给这个打不开的文件写一条 page=0 进度。
    st.pdf.view = PdfView::Reader;
    st.pdf.pageReady = false;
    st.pdf.pageCount = 1;
    st.pdf.page = 0;
    if (st.pdf.status.empty()) st.pdf.status = "PDF 打开失败";
    st.bookPath.clear();
    st.bookKind = -1;
    st.mode = RdMode::Pdf;
    st.tab = 1;
    st.fullRefresh = true;
    st.dirty = 1;
    return;
  }
  st.mode = RdMode::Pdf;
  st.pdf.view = PdfView::Reader;
  // openBook 的 rdRestoreProgress 已经把 page 还原到上次位置；进度由外壳帧尾统一落盘。
  bool ok = pdfLoadPage(st.pdf.page, true);
  // 打开页的编码不支持（扫描件常见：首页是 JPX）时顺延几页，别让用户一进来
  // 只看到错误。只在打开时做，手动翻到不支持页仍原样报错。
  for (int i = 0; i < 3 && !ok && st.pdf.page + 1 < st.pdf.pageCount; i++) {
    if (st.pdf.status.find("不支持") == std::string::npos) break;
    ok = pdfLoadPage(st.pdf.page + 1, true);
  }
  if (!ok && st.pdf.status.find("不支持") != std::string::npos) {
    st.pdf.status = "第 " + std::to_string(st.pdf.page + 1) + " 页编码不支持，按 → 翻页";
  }
}

bool pdfOpenDocumentForBook(const std::string &path) {
  int pc = 0;
  std::string title, err;
  if (!pdfioOpen(path, &pc, &title, &err)) {
    ESP_LOGE(TAG, "PDF 打开失败: %s (%s)", path.c_str(), err.c_str());
    st.pdf.status = err.empty() ? "PDF 打开失败" : err;
    return false;
  }
  st.pdf.pageCount = std::max(1, pc);
  st.pdf.page = 0;
  st.pdf.prefetchTried = -1;
  st.pdf.title = title.empty() ? pdfBaseName(path) : title;
  st.bookTitle = st.pdf.title;
  st.pdf.status.clear();
  st.pdf.pageReady = false;
  pdfLoadPrefs();   // 返回点恢复路径不经过 pdfOpenFrom，偏好也在这里落好
  return true;
}

void pdfCloseDocument() {
  pdfioClose();
  st.pdf.pageReady = false;
}

void pdfResetUi() {
  st.pdf.view = PdfView::List;
  st.pdf.list.clear();
  st.pdf.listSel = 0;
  st.pdf.path.clear();
  st.pdf.title.clear();
  st.pdf.status.clear();
  st.pdf.page = 0;
  st.pdf.pageCount = 0;
  st.pdf.pageReady = false;
  st.pdf.menuOpen = false;
  st.pdf.menuSel = 0;
  st.pdf.jumpOpen = false;
  st.pdf.jumpLen = 0;
  st.pdf.jumpDigits[0] = '\0';
  st.pdf.tapPending = false;
  st.pdf.zoom = 1.0f;
  st.pdf.panX = st.pdf.panY = 0.5f;
  st.pdf.panRoomX = st.pdf.panRoomY = 0;
  st.pdf.diffSteps = 0;
  st.pdf.prefetchTried = -1;
}

int pdfCurPageValue() { return st.pdf.page; }
int pdfPageCountValue() { return st.pdf.pageCount; }
bool pdfIsReaderView() { return st.pdf.view == PdfView::Reader; }

// ── 按键 / 触摸 ─────────────────────────────────────────────────────────
static void pdfReturnTo(PdfReturn ret) {
  st.pdf.menuOpen = false;
  st.pdf.jumpOpen = false;
  st.pdf.tapPending = false;
  st.fullRefresh = true;
  st.dirty = 1;
  switch (ret) {
    case PdfReturn::FileBrowser:
      st.tab = 1;
      st.mode = RdMode::FileBrowser;
      break;
    case PdfReturn::Recent:
      st.mode = RdMode::Recent;
      break;
    case PdfReturn::Apps:
    default:
      // 从应用页的 PDF 列表进来的：阅读页 Esc 回列表，而不是直接退出 PDF App。
      st.pdf.view = PdfView::List;
      st.pdf.pageReady = false;
      pdfScanList();
      st.mode = RdMode::Pdf;
      break;
  }
}

static void pdfHandleMenuKey(int key) {
  if (key == 0x1B || key == KEY_LONG_CONFIRM) {
    st.pdf.menuOpen = false;
    st.dirty = 1;
    return;
  }
  if (key == KEY_UP) {
    st.pdf.menuSel = (st.pdf.menuSel + pdfMenuRowCount() - 1) % pdfMenuRowCount();
    st.dirty = 1;
    return;
  }
  if (key == KEY_DOWN) {
    st.pdf.menuSel = (st.pdf.menuSel + 1) % pdfMenuRowCount();
    st.dirty = 1;
    return;
  }
  // 跳转行不是循环取值：左右键不做事，回车/点行才弹输入框。
  if (key == KEY_LEFT || key == KEY_RIGHT) {
    if (st.pdf.menuSel != 3) pdfMenuCycle(st.pdf.menuSel, key == KEY_LEFT ? -1 : +1);
    return;
  }
  if (key == '\n' || key == 0x0A || key == 0x0D) {
    int x = 0, y = 0;
    if (input_tap_xy(&x, &y)) {
      int row = -1;
      if (!pdfMenuHit(x, y, &row)) {   // 点卡片外：关菜单
        st.pdf.menuOpen = false;
        st.dirty = 1;
        return;
      }
      if (row >= 0) st.pdf.menuSel = row;
    }
    pdfMenuCycle(st.pdf.menuSel, +1);
  }
}

static bool pdfEffectiveZoomed() { return st.pdf.zoom > 1.01f; }

static void pdfPanBy(int dx, int dy) {
  // 余量由上一帧 blit 量好：只在这个方向真的溢出时才动，否则什么也不发生
  // （调用方用 panRoomX > 0 决定左右键是平移还是翻页）。
  bool moved = false;
  if (st.pdf.panRoomX > 0 && dx) {
    st.pdf.panX = std::clamp(st.pdf.panX - static_cast<float>(dx) / st.pdf.panRoomX, 0.0f, 1.0f);
    moved = true;
  }
  if (st.pdf.panRoomY > 0 && dy) {
    st.pdf.panY = std::clamp(st.pdf.panY - static_cast<float>(dy) / st.pdf.panRoomY, 0.0f, 1.0f);
    moved = true;
  }
  if (!moved) return;
  st.dirty = 1;
  st.fullRefresh = false;
}

static void pdfZoomCycle() {
  int step = 1;
  if (st.pdf.zoom > 0.99f && st.pdf.zoom < 1.5f) step = 2;
  else if (st.pdf.zoom < 2.5f) step = 3;
  else step = 1;
  st.pdf.zoom = static_cast<float>(step);
  st.pdf.panX = st.pdf.panY = 0.5f;
  st.pdf.diffSteps = 0;
  st.dirty = 1;
  st.fullRefresh = true;   // 倍率跳变，GC16 比差分干净
}

static void pdfHandleReaderKey(int key) {
  if (st.pdf.jumpOpen) {
    pdfHandleJumpKey(key);
    return;
  }
  if (st.pdf.menuOpen) {
    pdfHandleMenuKey(key);
    return;
  }
  if (key == 0x1B || key == KEY_LONG_CONFIRM) {
    pdfReturnTo(st.pdf.ret);
    return;
  }
  if (key == KEY_PAGE_UP || key == KEY_PAGE_DOWN) {
    pdfLoadPage(st.pdf.page + (key == KEY_PAGE_DOWN ? 1 : -1), true);
    return;
  }
  if (key == KEY_LEFT || key == KEY_RIGHT) {
    // 这个方向还有余量 → 平移（放大后、或"原始大小"下宽页溢出）；否则翻页。
    if (st.pdf.panRoomX > 0) {
      pdfPanBy(key == KEY_LEFT ? +40 : -40, 0);
    } else {
      pdfLoadPage(st.pdf.page + (key == KEY_RIGHT ? 1 : -1), true);
    }
    return;
  }
  if (key == KEY_UP || key == KEY_DOWN) {
    pdfPanBy(0, key == KEY_UP ? +40 : -40);
    return;
  }
  if (key == KEY_PINCH_IN) {
    st.pdf.zoom = std::max(1.0f, st.pdf.zoom / 1.25f);
    if (st.pdf.zoom <= 1.01f) st.pdf.panX = st.pdf.panY = 0.5f;
    st.pdf.diffSteps = 0;
    st.dirty = 1;
    st.fullRefresh = true;
    return;
  }
  if (key == KEY_PINCH_OUT) {
    st.pdf.zoom = std::min(3.0f, st.pdf.zoom * 1.25f);
    st.dirty = 1;
    st.fullRefresh = true;
    return;
  }
  if (key == '\n' || key == 0x0A || key == 0x0D) {
    int x = 0, y = 0;
    if (!input_tap_xy(&x, &y)) {
      pdfZoomCycle();
      return;
    }
    // 双击判定：第二次点按在 300ms 内、落点相近 → 以点击处为中心 2× / 回基准。
    const int64_t now = esp_timer_get_time();
    if (st.pdf.tapPending && now - st.pdf.tapAtUs <= kDoubleTapUs &&
        std::abs(x - st.pdf.tapX) <= kDoubleTapPx && std::abs(y - st.pdf.tapY) <= kDoubleTapPx) {
      st.pdf.tapPending = false;
      if (pdfEffectiveZoomed()) {
        st.pdf.zoom = 1.0f;
        st.pdf.panX = st.pdf.panY = 0.5f;
      } else {
        // 放大到 2×，让点按处的源像素留在原地。
        float outPanX = 0.5f, outPanY = 0.5f;
        const int effRot = (st.pdf.rotate % 360 + 360) % 360;
        if (pdfioAnchorPan(st.pdf.page, effRot, st.pdf.zoomMode, st.pdf.zoom, 2.0f, st.pdf.panX,
                           st.pdf.panY, 0, 0, g_rd.getScreenWidth(), statusTop(), x, y,
                           &outPanX, &outPanY)) {
          st.pdf.panX = outPanX;
          st.pdf.panY = outPanY;
        }
        st.pdf.zoom = 2.0f;
      }
      st.pdf.diffSteps = 0;
      st.dirty = 1;
      st.fullRefresh = true;
      return;
    }
    st.pdf.tapPending = true;
    st.pdf.tapX = x;
    st.pdf.tapY = y;
    st.pdf.tapAtUs = now;
    return;
  }
  if (key == KEY_TOUCH_DRAG) {
    int dx = 0, dy = 0;
    if (!input_drag_xy(&dx, &dy)) return;
    if (dx || dy) pdfPanBy(dx, dy);
    return;
  }
}

static void pdfHandleListKey(int key) {
  ListView lv = pdfListGeometry();
  if (listViewKey(lv, key)) {
    st.pdf.listSel = lv.sel;
    st.dirty = 1;
    return;
  }
  if (key == 0x1B || key == KEY_LONG_CONFIRM) {
    st.mode = RdMode::Apps;
    st.tab = 1;
    st.fullRefresh = true;
    st.dirty = 1;
    return;
  }
  if (key == '\n') {
    int x = 0, y = 0;
    if (input_tap_xy(&x, &y)) {
      const int row = listViewHitAt(pdfListGeometry(), y);
      if (row >= 0) st.pdf.listSel = row;
      else return;
    }
    if (st.pdf.list.empty()) return;
    const PdfListItem it = st.pdf.list[st.pdf.listSel];
    st.pdf.ret = PdfReturn::Apps;
    pdfOpenFrom(it.path, PdfReturn::Apps);
  }
}

void pdfHandle(int key) {
  if (st.pdf.view == PdfView::List) {
    pdfHandleListKey(key);
  } else {
    pdfHandleReaderKey(key);
  }
}

// 空闲预解码的中止/让位回调。每个进度点（解码每 64 行/256 个实例一次，≤40ms）
// 都补采一次触摸并盯一眼硬件键与蓝牙键盘；每 250ms 让出一次 CPU 喂看门狗。
// 返回 false = 有输入，预解码当场放弃（半成品缓存由解码层删掉）。
static int64_t s_pdfPrefetchYieldUs = 0;
static bool s_pdfPrefetchAborted = false;

static bool pdfPrefetchContinue(void *) {
  const int64_t now = esp_timer_get_time();
  if (s_pdfPrefetchYieldUs == 0 || now - s_pdfPrefetchYieldUs >= 250000) {
    s_pdfPrefetchYieldUs = now;
    vTaskDelay(1);
  }
  const bool cont = input_pending_key() == 0 && !g_bt.waitKey(0);
  if (!cont) s_pdfPrefetchAborted = true;
  return cont;
}

// 空闲帧：单击过 300ms 没等到第二击就兑现。区域按 x 分：左 30% 上一页、
// 右 30% 下一页、中间 40% 开/关设置浮层。
// 没有待定单击时做下一页预解码：新页第一次打开要 5~6s（JBIG2），这段落在空闲
// 时间里之后，顺着读翻页就只剩读缓存 + 刷屏（约 2~3s）。一有输入立刻中止。
bool pdfIdleTick() {
  if (st.mode != RdMode::Pdf || st.pdf.view != PdfView::Reader) return false;
  if (st.pdf.tapPending) {
    if (esp_timer_get_time() - st.pdf.tapAtUs < kDoubleTapUs) return false;
    st.pdf.tapPending = false;
    const int w = g_rd.getScreenWidth();
    if (st.pdf.tapX < w * 3 / 10) {
      pdfLoadPage(st.pdf.page - 1, true);
    } else if (st.pdf.tapX > w * 7 / 10) {
      pdfLoadPage(st.pdf.page + 1, true);
    } else {
      st.pdf.menuOpen = true;
      st.pdf.menuSel = 0;
      st.dirty = 1;
    }
    return true;
  }
  if (st.pdf.menuOpen || st.pdf.jumpOpen || !st.pdf.pageReady) return false;
  const int next = st.pdf.page + 1;
  if (next >= st.pdf.pageCount || next == st.pdf.prefetchTried) return false;
  const int effRot = (st.pdf.rotate % 360 + 360) % 360;
  if (pdfioPageCached(next, effRot)) {
    // 已经在缓存里（翻回来过）：不必解，记一笔别每拍再查一次。
    st.pdf.prefetchTried = next;
    return false;
  }
  s_pdfPrefetchAborted = false;
  const bool ok = pdfioPrefetchPage(next, pdfPrefetchContinue);
  if (ok || !s_pdfPrefetchAborted) st.pdf.prefetchTried = next;
  // 中途被输入打断：不记账，等手停下来后的空闲拍重试。不需要重绘。
  return false;
}
