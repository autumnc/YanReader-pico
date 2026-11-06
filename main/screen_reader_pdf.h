#pragma once
// ── PDF 阅读 App（页面图像型）─────────────────────────────────────────────
// 阅读模式「应用」页的第 6 枚入口。它不是一本书：文档解析、页缓存、视口采样全在
// screen_reader_pdf_cache.cpp，UI（列表/阅读/浮动设置）在 screen_reader_pdf.cpp。
// 这里只放三样东西：
//   · PdfState —— 挂在 RdState 上，和 imgZoom 那一族一样是静态会话状态；
//   · 阅读器外壳要调的入口（分发/渲染/空闲 tick/SD 掉卡/退出）；
//   · 缓存层入口（打开文档、取页几何、把一页画进 g_rd 的正文区）。
//
// 书身份用 bookKind = 5。RdMode 恒为 Pdf，绝不进 Reading 的排版/字体/目录管线；
// bookKind 只用于复用进度（reader_progress.txt 的 page 字段）、最近阅读与返回点。
//
// 解析器本身在 components/crossmux/lib/Pdf/，不认识 HalStorage —— 随机读经
// PdfSource 抽象，设备端实现也在这个 TU 里。

#include <cstdint>
#include <string>
#include <vector>

// view==List 是 PDF 文件选择页；Reader 是整页阅读。
enum class PdfView : uint8_t { List, Reader };

// 阅读页 Esc 的去处（打开来源）。List 只从应用页进，Esc 固定回 Apps。
enum class PdfReturn : uint8_t { Apps, FileBrowser, Recent };

struct PdfListItem {
    std::string path;
    std::string name;
};

// 一页的几何（缓存层解析结果）。crop 是归一化页面坐标 [x0,y0,x1,y1]，
// 自动裁边关闭时恒为 {0,0,1,1}；pxW/pxH 是旋转后的 PXC 像素尺寸。
struct PdfViewGeom {
    int pxW = 0;
    int pxH = 0;
    float crop[4] = {0, 0, 1, 1};
    bool cropValid = false;
};

struct PdfState {
    PdfView view = PdfView::List;
    PdfReturn ret = PdfReturn::Apps;

    // 列表
    std::vector<PdfListItem> list;
    int listSel = 0;

    // 当前文档
    std::string path;
    std::string title;
    std::string status;   // 底部状态/错误提示（空 = 正常页脚）
    int page = 0;
    int pageCount = 0;
    bool pageReady = false;

    // 视图偏好（持久化在 g_settings）
    int zoomMode = 1;   // 0 适合宽度 / 1 自适应 / 2 原始大小
    int cropMode = 0;   // 0 不裁 / 1 自动裁边
    int rotate = 0;     // 用户旋转：0/90/180/270（与页面固有 /Rotate 相加）
    float zoom = 1.0f;  // 相对当前缩放方式基准的倍率
    float panX = 0.5f;  // 可平移余量的比例 [0,1]（0=最左/最上，0.5=居中）
    float panY = 0.5f;
    // 最近一帧的可平移余量（屏幕像素，由 pdfioBlit 填）。0 = 那个方向上内容没溢出。
    int panRoomX = 0;
    int panRoomY = 0;

    // 刷新预算：连续差分帧计数，到 3 强制一次 FULL
    int diffSteps = 0;

    // 浮动设置菜单
    bool menuOpen = false;
    int menuSel = 0;

    // 跳转到页：数字输入框（从设置菜单第 4 行弹出）
    bool jumpOpen = false;
    int jumpLen = 0;
    char jumpDigits[8] = {};

    // 单击延迟判双击（空闲 tick 兑现）
    bool tapPending = false;
    int tapX = 0, tapY = 0;
    int64_t tapAtUs = 0;

    // 空闲预解码：已试过的下一页（-1 = 无）。被打断时不记账，下个空闲拍重试。
    int prefetchTried = -1;
};

// ── 阅读器外壳入口 ──────────────────────────────────────────────────────
void pdfEnterList();                                   // 应用页 → PDF 列表
void pdfOpenFrom(const std::string &path, PdfReturn ret);
bool pdfOpenDocumentForBook(const std::string &path);  // openBook(kind=5) 用
void pdfRender();
void pdfHandle(int key);
bool pdfIdleTick();       // 空闲帧：兑现待定单击；返回 true = 需要重绘
void pdfCloseDocument();  // 释放文档/文件句柄（SD 掉卡、退出阅读器）
void pdfResetUi();        // 清列表与阅读态（不碰进度表）
// 按 st.pdf.page 解码并显示当前页（返回点/最近阅读恢复用；pdfLoadPage 的公开壳）。
void pdfEnsureCurrentPage(bool forceFull);

int pdfCurPageValue();    // 给 totalPages()/curPage()/返回点用
int pdfPageCountValue();
bool pdfIsReaderView();

// ── 缓存/解码层入口（screen_reader_pdf_cache.cpp）────────────────────────
bool pdfioOpen(const std::string &path, int *pageCount, std::string *title,
               std::string *err);
void pdfioClose();
bool pdfioPageGeom(int page, int effRotate, PdfViewGeom *out, std::string *err);
// 确保这一页（指定有效旋转）已解码成 PXC 缓存。abort 返回 true 时中途放弃。
bool pdfioEnsurePage(int page, int effRotate, std::string *err);
// 这一页的 PXC 是否已在缓存里（不触发解码）。用来决定要不要先推「正在载入」帧。
bool pdfioPageCached(int page, int effRotate);
// 空闲预解码：只把 page 的 PXC 缓存解出来，不切当前页。progress 返回 false = 中止
// （有输入），缓存半成品会删掉，下个空闲拍可重试。
using PdfPrefetchProgress = bool (*)(void *);
bool pdfioPrefetchPage(int page, PdfPrefetchProgress progress);
// 把一页按当前视口画进 g_rd 的 body 区域。返回 false 时 err 填原因。
bool pdfioBlit(int page, int effRotate, int zoomMode, float zoomMul, float panX, float panY,
               int areaX, int areaY, int areaW, int areaH, std::string *err);
// 双击放大锚点：把屏幕点 (tapX,tapY) 下的源像素在 zoom=targetZoom 时留在原地，
// 返回新的归一化 pan（0=最左/上，1=最右/下）。
bool pdfioAnchorPan(int page, int effRotate, int zoomMode, float currentZoom, float targetZoom,
                    float panX, float panY, int areaX, int areaY, int areaW, int areaH,
                    int tapX, int tapY, float *outPanX, float *outPanY);
