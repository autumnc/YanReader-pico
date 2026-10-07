#pragma once

#include <string>

#include "pjournal_app.h"
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

void screen_reader_init();
void screen_reader_exit();
AppState screen_reader_handle(int key, ScreenContext &ctx);

// 进入 / 离开阅读模式（main.cpp 的 kScreens 生命周期钩子）。就是 init/exit 的适配层——
// 阅读器的"进入"必须真的重建（它自己用 s_return 记住上次停在哪一屏，见
// rdRestoreReturnPoint），"离开"必须真的收尾（记进度 + 释放书对象 + 还方向）。
// 做成两个新名字是为了对上 Screen 表的函数签名，语义一字未改。
void screen_reader_enter(ScreenContext &ctx);
void screen_reader_leave(AppState next);

// ── 待机表盘「书籍封面」的两个出口 ───────────────────────────────────────
// 只有 screen_reader.cpp 知道进度表、封面缓存路径的算法和封面解码器，所以这两件事
// 都由它出面。做成"取数"而不是"绘制"：待机表盘画在 ui_render 的工作缓冲上，而这里
// 能用的画布只有 g_rd（绑在 front_fb 上，见 ui_render.cpp 里那条不变式）。

// 最后阅读的一本书的信息（reader_progress.txt 的第一条，该文件**最新在前**，
// 翻开一本书或翻页都会把当前书挪到表头）。没有记录（没读过书 / 卡没插）返回 false。
struct StandbyBookInfo {
    std::string coverBmp;   // 该书封面 BMP 的缓存路径（**可能还没生成**，调用方自己判存在）
    std::string title;      // 书名（统计里没有就退回文件名）
    std::string chapter;    // 当前章节名（还没读到章节 / 统计里没有就是空串）
    int percent = 0;          // 全书进度百分比
    int chapterPercent = 0;   // 本章进度百分比
    uint64_t readingMs = 0;   // 这本书的累计阅读时长
};
bool readerLastBookInfo(StandbyBookInfo &out);

// 待机封面表盘的**版式**：封面框 + 信息区（竖屏在封面下方，横屏在封面右侧）。
// 生成端（rdBuildStandbyCoverForOpenBook）和解码端（待机表盘的 drawCoverFace）必须用
// **同一个封面框**，否则 1:1 直拷那条快路径永远命中不了，而且缓存文件的尺寸会跟画出来
// 的框对不上。所以版式的计算只有这一份，两边都调它。
//
// 封面框取 2:3（与常见封面一致，下采样比最小、最清晰）；框的大小为"去掉信息区之后
// 剩下的地方都给它"：竖屏信息区在下（5 行高），横屏信息区在右。横屏之所以不按竖屏那
// 样上下排，是因为 1216×684 把下面切掉五行之后只剩两百来像素放封面 —— 那点高度里
// 2:3 的封面只有 160px 宽，还没巴掌大。
// / The standby cover layout: cover box plus the info region (below in portrait, right in
// landscape). Generator and decoder must use the same box, so it is computed once here.
struct StandbyCoverLayout {
    int boxX = 0, boxY = 0, boxW = 0, boxH = 0;          // 封面框
    bool sideBySide = false;                             // true=横屏：信息在封面右侧
    int infoX = 0, infoY = 0, infoW = 0, infoH = 0;      // 信息区（版式的另一半）
};
void readerStandbyCoverLayout(StandbyCoverLayout &out);

// 把封面按"装进 boxW×boxH"缩放成 0..15 灰度（15=白）写进调用方给的 out，并回吐实际
// 尺寸 dw/dh 与在框内的居中偏移 ox/oy。out 至少要 boxW*boxH 字节 —— 一张整屏封面约
// 300KB，**必须取自 PSRAM**（内部 RAM 挤不出这么大一块，见 internal-ram-squeeze）。
// **只缩不放**：源图比框小的那一边按原尺寸来（dw/dh 就是源尺寸），多出来的框留白。
// 只读只算不画，像素由调用方自己 blit。
bool readerCoverScale(const std::string &bmpPath, int boxW, int boxH, uint8_t *out, size_t outCap,
                      int &dw, int &dh, int &ox, int &oy);

// ── 待机表盘「图片」的两个出口 ───────────────────────────────────────────
// 用户在文件管理里选中的那张图（设置键 standby_image，存的是 SD 上的**原图**路径）。
// 待机那一刻不解码原图（几百万像素的 JPEG 要好几秒，而这是休眠前的最后一屏），
// 所以选中时就按当前屏尺寸解成一张 Gray8 BMP 缓存，待机只做"读 BMP + 铺屏"。
// 缓存名带屏尺寸：转了屏就换一张，转回来还能命中老的那张。
//
// readerStandbyImage：给待机表盘取"现在该铺哪张 BMP"。没选图时返回 false。
// bmpPath 是缓存路径（**可能还没生成**：转屏后、或选了图但缓存被清掉了，调用方自己
// 判 Storage.exists）；srcPath 是原图路径，画不出来时用来提示。
bool readerStandbyImage(std::string &bmpPath, std::string &srcPath);

// 把 srcPath 解成当前屏尺寸的待机缓存，并把设置键 standby_image 指向它、表盘切到
// 「图片」（"设为待机画面"这个动作的语义就是"以后待机看这张"）。失败时 err 填原因。
// 会**原地解一张大图**（秒级）：调用方要先给一帧"正在…"的提示再进。
bool readerSetStandbyImage(const std::string &srcPath, std::string &err);

// ── 网页（/api/set_standby）那条路要拆成两步 ─────────────────────────────
// 原因只有一条：栈。解一张几百万像素的 JPEG 要十几 KB 栈，而 httpd 任务只有 8KB。
// readerRequestStandbyImage 是**投递**：立刻改设置键（语义马上生效），把"解图"记成
// 一张待办；可以在 httpd 任务上安全调用，几十微秒就返回。
// readerStandbyPump 是**取件**：由主循环每拍调一次（16KB 栈的主任务），有待办才干活。
void readerRequestStandbyImage(const std::string &srcPath);
void readerStandbyPump();

// ── SD 热插拔（运行中拔卡/插卡）的两条收尾 ───────────────────────────────
// 判断"卡还在不在"只有 main 侧一处轮询（main.cpp 的 sdHotplugTick），但画面得由
// 阅读器自己收 —— 卡最常是在这里被拔掉的，而那时正文页正指着一堆失效的文件句柄。
// 两个函数都**自己当场渲染**，且**只在当前界面就是阅读器时**才允许被调（否则会把
// 用户正在看的别的界面盖掉）。
//
// on_sd_lost：卡掉了。有书开着就先按"回书架"收尾、再释放书对象（留着就是留一堆
// 通向已经不存在的卡的句柄），清空书架列表，提示一条；card_absent=false 表示"卡在
// 但文件系统没挂上"（提示语不同）。返回 true = 当时确实关掉了一本书。
// on_sd_ready：卡回来了且已挂载。重扫书架并把结果提示出来。
bool screen_reader_on_sd_lost(bool card_absent);
void screen_reader_on_sd_ready();
