// Flomo 笔记界面。结构照着 screen_inspiration.cpp（同样是"列表 + 详情 + 搜索 +
// 帮助 + 借用写作编辑器改正文"），同步部分照 ../Flomo/src/main.cpp 的 syncStore，
// 区别只有两点（用户要求）：
//   1) **没有自动同步**——新建/编辑/删除只落本地并打 dirty 标记，同步要手动触发
//      （列表里的云图标 FAB，或 R 键）。
//   2) 同步时先把 WiFi 连上、再逐段报进度（上传第几条 / 拉取第几页）。
// 登录信息直接用设置里的 flomo_email/flomo_pass（与「发送到 Flomo」共用），
// 换到的 token 写回设置，下次免登录。

#include "screen_flomo.h"

#include "flomo_api.h"
#include "flomo_db.h"
#include "font_renderer.h"
#include "font_store.h"   // font_store_get_path：折行缓存键要带"当前内容字体"
#include "icon_font.h"
#include "ime/IME.h"
#include "ui/ime_field.h"  // 输入框绑定：落串/退格/光标的 UTF-8 算术（与写作/计划/阅读共用一份）
#include "screen_editor.h"
#include "settings_manager.h"
#include "ui_helpers.h"
#include "ui/list_view.h"  // listPageStep：手写列表的翻页步长（一屏行数 / 没得翻 = 0）
#include "ui/scroll_text.h"
#include "wifi_manager.h"
#include "hw/input.h"
#include "editor_vk.h"   // 虚拟键盘：检索框没连蓝牙键盘时的唯一输入途径
#include "text_sel.h"    // 单行输入框的触摸选字 / 粘贴板（三模式共享底层件）
#include "clipboard.h"   // 长按菜单的「复制内容」

#include <cstdio>
#include <cstring>
#include <ctime>
#include <algorithm>
#include <atomic>

#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include "u8g2_shim.h"

extern "C" {
    // 图标字体不走 u8g2，直接写 4bpp 帧缓冲（与 screen_editor 同一手法）。
}

#define FLOMO_DIR "/sdcard/flomo"
#define FLOMO_TMP_PREFIX "/sdcard/pjournal/__flomo_"
#define FLOMO_SYNC_ICON 0xF063F   // MENU 组里的 md-cloud-sync，已在图标子集内

enum FlomoMode { FM_LIST, FM_DETAIL, FM_CONFIRM_DELETE, FM_SEARCH, FM_HELP, FM_ITEM_MENU };

static struct {
    FlomoMode mode = FM_LIST;
    Store store;
    std::vector<int> filtered;   // 可见笔记在 store.memos 里的下标

    int sel = 0;
    int scroll = 0;
    int detailScroll = 0;
    int helpScroll = 0;
    int menuSel = 0;             // FM_ITEM_MENU 里高亮的是第几项

    std::string query;
    std::string tag;
    std::string status;

    AppState returnTo = APP_MAIN;

    // 编辑器交接：pendingTemp 是 journal 目录下的临时文件名，
    // pendingSlug 为空 = 新建，否则是正在编辑的那条笔记的 slug。
    std::string pendingTemp;
    std::string pendingSlug;
    int editorSeq = 0;

    // 搜索输入
    std::string searchBuf;
    int searchCur = 0;
    bool searchIme = false;

    // ── 空转记账（见 screen_flomo_handle 开头那段）───────────────────────
    // drawn = 这一屏当前这幅画已经提交过；drawnW/H = 画它时的屏幕几何
    // （自适应转屏后几何一变就作废，得重画）；toastWas = 上一幅画上带着浮标，
    // 浮标到点熄灭后要再画一帧把它擦掉。
    bool drawn = false;
    int drawnW = 0, drawnH = 0;
    bool toastWas = false;
} g;

// 检索框的 ImeField 形态（带真实光标，可左右移）。
static ImeField flomoSearchField() { return ImeField{&g.searchBuf, &g.searchCur}; }

// ── 小工具 ────────────────────────────────────────────────────────────────

static bool isLocalOnly(const Memo &m) {
    return m.pendingOp == "create" || m.slug.rfind("local-", 0) == 0;
}

static std::string nowStamp() {
    char buf[32];
    std::time_t t = std::time(nullptr);
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", std::localtime(&t));
    return buf;
}

static void loadLocal() {
    MemoDb db(FLOMO_DIR);
    g.store.memos.clear();
    db.load(g.store);
    if (g.store.token.empty()) g.store.token = g_settings.flomoToken();
}

static void saveLocal() {
    MemoDb db(FLOMO_DIR);
    if (g.store.token.empty()) g.store.token = g_settings.flomoToken();
    db.save(g.store);
}

static void rebuildFilter() {
    g.filtered.clear();
    for (int i = 0; i < (int)g.store.memos.size(); ++i) {
        const Memo &m = g.store.memos[i];
        if (m.deleted) continue;
        if (!g.query.empty() && m.contentText.find(g.query) == std::string::npos) continue;
        if (!g.tag.empty() && std::find(m.tags.begin(), m.tags.end(), g.tag) == m.tags.end()) continue;
        g.filtered.push_back(i);
    }
    if (g.sel >= (int)g.filtered.size()) g.sel = std::max(0, (int)g.filtered.size() - 1);
    if (g.sel < 0) g.sel = 0;
}

static std::string firstLine(const std::string &s) {
    size_t p = s.find('\n');
    std::string l = p == std::string::npos ? s : s.substr(0, p);
    return l;
}

// 按像素宽度折行（中文没有词边界，逐字符量宽；ASCII 空格作为优先断点）。
static std::vector<std::string> wrapText(const std::string &text, int maxW) {
    std::vector<std::string> out;
    std::string para;
    size_t start = 0;
    for (size_t i = 0; i <= text.size(); ++i) {
        if (i == text.size() || text[i] == '\n') {
            para = text.substr(start, i - start);
            start = i + 1;
            int pos = 0, len = (int)para.size();
            if (len == 0) { out.push_back(""); continue; }
            while (pos < len) {
                int end = pos, lastBreak = -1;
                while (end < len) {
                    std::string sub = para.substr(pos, end - pos + 1);
                    if (g_font.textWidth(sub.c_str()) > maxW) break;
                    if (para[end] == ' ') lastBreak = end + 1;
                    end++;
                }
                if (end >= len) { out.push_back(para.substr(pos)); break; }
                if (lastBreak > pos) { out.push_back(para.substr(pos, lastBreak - pos)); pos = lastBreak; }
                else if (end > pos) { out.push_back(para.substr(pos, end - pos)); pos = end; }
                else { out.push_back(para.substr(pos, 1)); pos++; }
            }
        }
    }
    return out;
}

static std::string readTempFile(const std::string &path) {
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) return "";
    std::string out;
    char buf[512];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    fclose(f);
    return out;
}

// ── 同步：跑在后台任务里 ──────────────────────────────────────────────────
//
// 一趟同步要连 WiFi（最多 10 秒轮询）、可能还要登录，然后逐条推本地改动、逐页拉远端
// —— 每次 HTTP 的墙钟上限是 45s（见 flomo_api.cpp 的 deadline_ms），整趟最坏几十秒。
// 原来这一整套是在**主任务**里跑完才回来的：那段时间主循环根本出不来，触摸一个都采
// 不到，屏上表现就是"按了云图标 / R 之后只剩进度，点哪儿都没反应"。现在整趟搬到
// worker 任务（与 WebDAV 同步、发送到 flomo 同一套做法，见 main.cpp 的 webdavSyncTask
// 与 AsyncUiState），本屏只负责把 worker 报上来的进度画出来。
//
// 两个任务之间**不共享库**：worker 自己从磁盘 load 一份 Store（flomoSyncWork 只碰它
// 和磁盘），推拉完整体原子落盘（MemoDb::save → flomoSafeWriteFile），主任务在"结束"
// 那一拍才把库重新读一遍（loadLocal）——所以不用给 g.store 加锁，也不会画到半路的库。
// 共享的只有下面这把锁和两个字符串。
//
// 也因此这一屏**可以中途走人**（Esc）：worker 不依赖它，写的是磁盘，回来 loadLocal
// 照样是新的。原来那条路是走不掉的 —— 同步整个卡在主任务里，按键轮不到处理。
enum FlomoSyncState { FSYNC_IDLE = 0, FSYNC_RUNNING, FSYNC_DONE };
static std::atomic<int> s_syncState{FSYNC_IDLE};
static SemaphoreHandle_t s_syncMux = nullptr;
static std::string s_syncMsg;      // 进度串（worker 写、主任务读来画）
static std::string s_syncResult;   // 收尾文案：失败原因是它；成功时留空，条数由主任务数
static bool s_syncOk = false;

static void ensureSyncMux() {
    if (!s_syncMux) s_syncMux = xSemaphoreCreateMutex();
}
static void syncLock()   { ensureSyncMux(); if (s_syncMux) xSemaphoreTake(s_syncMux, portMAX_DELAY); }
static void syncUnlock() { if (s_syncMux) xSemaphoreGive(s_syncMux); }

// 报一段进度（worker 侧）。这里只写串，画屏是主任务的事 —— worker 一行都不画，
// 它没有帧缓冲的所有权，也不该有。
static void syncPublish(const std::string &msg) {
    syncLock();
    s_syncMsg = msg;
    syncUnlock();
}

// 连 WiFi（worker 侧版：ensure_wifi_connected() 不回调，进度自己报；失败写 status）。
static bool ensureWifiWork(std::string &status) {
    if (g_wifi.isConnected()) return true;
    std::string ssid = g_settings.wifiSsid();
    std::string pass = g_settings.wifiPassword();
    if (ssid.empty()) { status = "未配置 WiFi（先到设置填写）"; return false; }
    g_wifi.begin();
    if (!g_wifi.connect(ssid.c_str(), pass.c_str())) { status = "WiFi 连接失败"; return false; }
    for (int i = 0; i < 100; i++) {
        if (g_wifi.isConnected()) return true;
        static const char *dots[] = {"...", "....", ".....", ".."};
        syncPublish(std::string("正在连接 WiFi") + dots[i % 4]);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    status = "WiFi 连接超时";
    return false;
}

// 进度界面（主任务侧）：居中一行提示，每换一段就重画一次，让用户看到进度。
static void drawSyncShared() {
    std::string msg;
    syncLock();
    msg = s_syncMsg;
    syncUnlock();
    ui_clear();
    int base = (SCREEN_H - FONT_H) / 2;
    ui_draw_text_content_centered(base, "Flomo 同步", false);
    ui_draw_text_content_centered(base + FONT_H + 6, msg.c_str());
    ui_draw_text_content_centered(STATUS_Y, "请稍候…");
    ui_commit();
}

// 从响应里挑出服务端返回的 memo（Flomo 不同接口包的层不一样）。
static Memo memoFromResult(const ApiResult &r) {
    Memo m = memoFromJson(r.data);
    if (!m.slug.empty()) return m;
    m = memoFromJson(r.data["memo"]);
    if (!m.slug.empty()) return m;
    return memoFromJson(r.data["data"]);
}

static void removeMemoBySlug(std::vector<Memo> &memos, const std::string &slug) {
    if (slug.empty()) return;
    memos.erase(std::remove_if(memos.begin(), memos.end(),
                               [&](const Memo &m) { return m.slug == slug; }),
                memos.end());
}

// 手动同步的实际工作：先把本地 dirty 的改动推上去，再逐页拉取远端较新的笔记。
// **只碰传进来的 work、设置和磁盘** —— 不画屏、不碰 g.store、不碰主任务的任何状态，
// 它就是给 worker 任务跑的（见下面 flomoSyncTask）。失败时 status 里是原因；成功时
// status 留空（"同步完成 N 条"由主任务按重新过滤后的结果数生成）。
static bool flomoSyncWork(Store &work, std::string &status) {
    auto saveWork = [&]() {
        MemoDb db(FLOMO_DIR);
        db.save(work);
    };

    if (!ensureWifiWork(status)) return false;

    std::string token = g_settings.flomoToken();
    if (token.empty()) {
        std::string email = g_settings.flomoEmail();
        std::string pass = g_settings.flomoPassword();
        if (email.empty() || pass.empty()) {
            status = "未配置 Flomo 账号（进设置填写）";
            return false;
        }
        syncPublish("正在登录 Flomo…");
        ApiResult r = FlomoApi::login(email, pass);
        if (!r.ok) { status = "登录失败: " + r.message; return false; }
        token = r.data["access_token"].asString();
        if (token.empty()) { status = "登录未返回令牌"; return false; }
        // 单键一个文件、自带锁 + 原子写（settings_manager.cpp:52），从 worker 写安全。
        g_settings.setFlomoToken(token);
        work.token = token;
    }

    FlomoApi api(token);

    // ① 推送本地改动
    int dirtyTotal = 0;
    for (auto &m : work.memos) if (m.dirty) dirtyTotal++;
    int dirtyDone = 0;
    for (auto &m : work.memos) {
        if (!m.dirty) continue;
        dirtyDone++;
        ApiResult r;
        std::string opName;
        if (m.pendingOp == "create") { opName = "上传"; r = api.createMemo(m.contentText); }
        else if (m.pendingOp == "update") { opName = "更新"; r = api.updateMemo(m.slug, m.contentText); }
        else if (m.pendingOp == "delete") {
            if (isLocalOnly(m)) { m.dirty = false; m.pendingOp.clear(); continue; }
            opName = "删除";
            r = api.deleteMemo(m.slug);
        } else { continue; }

        char prog[64];
        snprintf(prog, sizeof(prog), "%s本地改动 %d/%d…", opName.c_str(), dirtyDone, dirtyTotal);
        syncPublish(prog);

        if (!r.ok) {
            saveWork();
            status = opName + "失败: " + r.message;
            // 半路退出也要落盘：这一趟可能已经替换过若干 memo（m = remote）。库与主任务
            // 的索引表对不上没关系 —— 主任务收工时才 loadLocal + rebuildFilter，它看到的
            // 永远是磁盘上这份完整的库。
            return false;
        }
        if (m.pendingOp == "create") {
            Memo remote = memoFromResult(r);
            if (remote.slug.empty()) {
                // 服务端没回 memo：本地这条已提交，保留内容清掉标记即可。
                m.dirty = false;
                m.pendingOp.clear();
            } else {
                m = remote;
            }
        } else if (m.pendingOp == "update") {
            Memo remote = memoFromResult(r);
            if (!remote.slug.empty()) m = remote;
            else { m.dirty = false; m.pendingOp.clear(); }
        } else {
            m.dirty = false;
            m.pendingOp.clear();
        }
    }

    // 已删且没有未同步标记的本地条目真正丢掉。
    work.memos.erase(std::remove_if(work.memos.begin(), work.memos.end(),
                                    [](const Memo &m) { return m.deleted && !m.dirty; }),
                     work.memos.end());

    // ② 拉取远端（分页，最多 30 页兜底）
    std::string slug, updated;
    for (int page = 1; page <= 30; ++page) {
        char prog[48];
        snprintf(prog, sizeof(prog), "拉取笔记 第 %d 页…", page);
        syncPublish(prog);
        ApiResult r = api.listPage(slug, updated);
        if (!r.ok) {
            saveWork();
            status = "刷新失败: " + r.message;
            // 上面已经 erase 掉「已删且无标记」的本地条目、也可能 removeMemoBySlug 过，
            // 库比进来时短了 —— 落盘就是落下这一版。主任务收工时按它重建索引，不会
            // 拿旧下标去 memos[] 越界（原来在主任务里跑，靠的正是返回前 rebuildFilter
            // 那一句；现在这一步搬到收工那一拍，见 screen_flomo_handle）。
            return false;
        }
        if (!r.data.isArray() || r.data.size() == 0) break;
        for (size_t i = 0; i < r.data.size(); ++i) {
            Memo m = memoFromJson(r.data[i]);
            if (m.deleted) removeMemoBySlug(work.memos, m.slug);
            else upsertMemo(work.memos, m);
            slug = m.slug;
            updated = m.updatedAt;
        }
        if (r.data.size() < 200) break;
    }

    work.lastSync = nowStamp();
    sortMemos(work.memos);
    saveWork();
    return true;
}

// worker 任务：自己 load 一份库 → 干活 → 落盘 → 发结果。全程不画屏。
static void flomoSyncTask(void *arg) {
    (void)arg;
    const char *TAG = "FlomoSync";
    const int64_t t0 = esp_timer_get_time();
    ESP_LOGI(TAG, "同步开始");

    Store work;
    {
        MemoDb db(FLOMO_DIR);
        db.load(work);
        if (work.token.empty()) work.token = g_settings.flomoToken();
    }
    syncPublish("正在准备…");

    std::string status;
    const bool ok = flomoSyncWork(work, status);
    // 栈余量一起打：这个任务的栈是照着 webdav_sync 抄的 12288，而它比那边多做一件
    // 事 —— load 整个库（主任务上这一步跑在 16K 栈里）。余量太薄就得往上加。
    ESP_LOGI(TAG, "同步结束 ok=%d 耗时 %d ms 栈余 %u %s", (int)ok,
             (int)((esp_timer_get_time() - t0) / 1000),
             (unsigned)uxTaskGetStackHighWaterMark(nullptr), status.c_str());

    syncLock();
    s_syncResult = status;
    s_syncOk = ok;
    syncUnlock();
    s_syncState.store(FSYNC_DONE, std::memory_order_release);
    vTaskDelete(nullptr);
}

// 手动同步的入口（云图标 / R 键）：只起任务，这一拍就回来 —— 主循环下一拍照常轮询触摸。
static void flomoTriggerSync() {
    if (s_syncState.load(std::memory_order_acquire) != FSYNC_IDLE) return;   // 已经在跑了
    ensureSyncMux();
    syncLock();
    s_syncMsg.clear();
    s_syncResult.clear();
    s_syncOk = false;
    syncUnlock();
    s_syncState.store(FSYNC_RUNNING, std::memory_order_release);

    TaskHandle_t h = nullptr;
    // 优先级 1（与 webdav_sync / flomo_send 一致：让主循环同优先级轮转，别抢它的键）。
    //
    // 栈 8192 **不能照抄 webdav_sync 的 12288**：那个数落在内部 RAM 上，而这块内存常态
    // 就是"最大空闲块 ~10KB"（见 ui_render.cpp 那段注释和启动后的 Heap 行：
    // `int free=20407 largest=10240`）—— 12288 连分配都过不去，同步会直接报"系统繁忙"。
    // 8192 是 flomo_send 那个任务的值（同样走 FlomoApi / HTTPS，网络这一头的开销是同
    // 一份），本任务多出来的是整个库的 load/save，而那个解析器是浅递归 + 堆上 DOM
    // （flomo_json.cpp 的 parseValue 只按嵌套层数递归，一层一个栈帧），1~2KB 的量级。
    // 实际余量看任务收尾打的那行"栈余 N"：低于 1KB 就得回来重算。
    if (xTaskCreate(flomoSyncTask, "flomo_sync", 8192, nullptr, 1, &h) != pdPASS) {
        syncLock();
        s_syncResult = "系统繁忙,请重试";
        s_syncOk = false;
        syncUnlock();
        s_syncState.store(FSYNC_DONE, std::memory_order_release);
    }
}

// ── 绘制 ──────────────────────────────────────────────────────────────────

// 右下角竖成一列的浮动按钮：slot 0 在最下（云同步/编辑），往上每格让开 8px。
// 同一套几何给绘制与命中共用，改一处两边一起动。
static int flomoFabPx() { return 52; }
static int flomoFabX() { return SCREEN_W - 12 - flomoFabPx(); }
static int flomoFabY(int slot) {
    return STATUS_BAR_Y - 10 - flomoFabPx() - slot * (flomoFabPx() + 8);
}
static bool flomoFabHit(int slot, int x, int y) {
    const int pad = 8, b = flomoFabPx();
    return x >= flomoFabX() - pad && x <= flomoFabX() + b + pad &&
           y >= flomoFabY(slot) - pad && y <= flomoFabY(slot) + b + pad;
}
// 整列占掉的竖向高度（详情页正文要避开它，否则末尾几行右端被按钮压住）。
static int flomoFabColH() { return 2 * flomoFabPx() + 8 + 10; }

// 按钮底：**白底实心** + 黑框。实心是为了压住底下的列表文字——只画一圈框的话
// 字会从按钮里透出来绞在一起。
static void flomoFabBox(int slot) {
    const int b = flomoFabPx(), x = flomoFabX(), y = flomoFabY(slot);
    u8g2_SetDrawColor(g_u8g2, 1);
    u8g2_DrawBox(g_u8g2, x, y, b, b);
    u8g2_SetDrawColor(g_u8g2, 0);
    u8g2_DrawFrame(g_u8g2, x, y, b, b);
}

// 图标字体不走 u8g2，直接写 4bpp 帧缓冲（与 screen_editor 同一手法）。
static void flomoFabIcon(int slot, uint32_t cp) {
    const int b = flomoFabPx(), x = flomoFabX(), y = flomoFabY(slot);
    uint8_t *fb = g_u8g2 ? u8g2_GetBufferPtr(g_u8g2) : nullptr;
    if (!fb) return;
    int gpx = b - 16;
    icon_font_draw_sized(fb, x + (b - gpx) / 2, y + (b - gpx) / 2, gpx, gpx, cp, false, gpx);
}

// 列表的"手动同步"按钮。
static void drawSyncFab() {
    flomoFabBox(0);
    flomoFabIcon(0, FLOMO_SYNC_ICON);
}

// 列表的"新建"按钮。图标子集里没有 plus 字形，为它重裁一次字体不划算，
// 两条实心矩形更省事（照抄阅读模式文件浏览页的 fbDrawNewFab）。
static void drawNewFab() {
    flomoFabBox(1);
    const int b = flomoFabPx(), x = flomoFabX(), y = flomoFabY(1);
    const int cx = x + b / 2, cy = y + b / 2;
    const int arm = b / 2 - 10;                 // 笔画半长
    const int t = std::max(2, b / 12);          // 笔画粗细
    u8g2_SetDrawColor(g_u8g2, 0);
    u8g2_DrawBox(g_u8g2, cx - arm, cy - t / 2, 2 * arm, t);
    u8g2_DrawBox(g_u8g2, cx - t / 2, cy - arm, t, 2 * arm);
}

// 详情页的"编辑"按钮。0x270E(✎) 在 icon_font.c 里被重映射到 md-pencil 0xF03EB，
// 该字形已在图标子集内（启动日志里实测画得出），不用新裁字形。
static void drawEditFab() {
    flomoFabBox(0);
    flomoFabIcon(0, 0x270E);
}

// 详情页的"删除"按钮：垃圾桶同样程序化画（横盖 + 提手 + 桶身边框 + 两条竖棱）。
static void drawTrashFab(int slot) {
    flomoFabBox(slot);
    const int b = flomoFabPx(), x = flomoFabX(), y = flomoFabY(slot);
    const int m = b / 6;                 // 四周留白
    const int t = std::max(2, b / 16);   // 盖/棱的厚度
    u8g2_SetDrawColor(g_u8g2, 0);
    u8g2_DrawBox(g_u8g2, x + b / 2 - b / 12, y + m - t, b / 6, t);   // 提手
    u8g2_DrawBox(g_u8g2, x + m - 2, y + m, b - 2 * m + 4, t);        // 横盖
    const int bx = x + m + 3, by = y + m + t + 2;
    const int bw = b - 2 * m - 6, bh = (y + b - m) - by;
    if (bw > 4 && bh > 6) {
        u8g2_DrawFrame(g_u8g2, bx, by, bw, bh);                      // 桶身
        const int ribH = bh - 10;
        if (ribH > 2) {
            u8g2_DrawBox(g_u8g2, bx + bw / 2 - 6, by + 5, t, ribH);  // 桶内两条竖棱
            u8g2_DrawBox(g_u8g2, bx + bw / 2 + 4, by + 5, t, ribH);
        }
    }
}

static std::string listStatusLeft() {
    if (!g.status.empty()) return g.status;
    if (g.store.token.empty() && g.filtered.empty()) return "N新建  R同步  /搜索  ?帮助";
    return "Enter详情  N新建  E编辑  D删除  R同步";
}

// 列表的画面本体（不含提交）：长按菜单要在这上面盖浮层再一次性提交。
// ── 翻页步长（触摸上下滑 = 整页翻）──────────────────────────────────────
// 一屏的行数/行数，与对应的绘制函数**同一个基准**（那边是排版，这边是翻页，
// 两处各写一遍式子迟早会漂）。行数 < 1 时抬到 1，调用方不必再判。
static int flomoListVis() {
    const int y = FONT_H + 4 + LINE_SPACING;  // 与 drawListBody/listRowAtY 的首行同源
    int v = (STATUS_Y - y + LINE_SPACING - 1) / LINE_SPACING;
    return v < 1 ? 1 : v;
}

static int flomoSearchVis() {
    const bool vk = editorVkVisible();
    const bool composing = g.searchIme && g_ime.composing() && !vk;
    const int listY = FONT_H + 4 + LINE_SPACING;
    const int listMaxY = vk ? (editorVkTop() - LINE_SPACING)
                            : (composing ? imeFullscreenPanelTopY() - LINE_SPACING : SCREEN_H);
    int v = (listMaxY - listY + LINE_SPACING - 1) / LINE_SPACING;  // 与 drawSearch 同式
    return v < 1 ? 1 : v;
}

static int flomoDetailVis() {
    const int top = (FONT_H + 4) + LINE_SPACING;  // sepY + LINE_SPACING
    int v = (STATUS_BAR_Y - flomoFabColH() - top) / LINE_SPACING;  // 与 drawDetailBody 同式
    return v < 1 ? 1 : v;
}

static int flomoHelpVis() {
    const int contentY = ui_title_baseline() + g_font.descent() + 12;
    int v = (STATUS_Y - contentY) / LINE_SPACING;  // 与 drawHelp 同式
    return v < 1 ? 1 : v;
}

static ListView flomoListView(bool search) {
    ListView lv;
    lv.sel = g.sel;
    lv.first = g.scroll;
    lv.count = (int)g.filtered.size();
    lv.rows = search ? flomoSearchVis() : flomoListVis();
    lv.top = FONT_H + 4 + LINE_SPACING - FONT_H - 2;
    lv.itemH = LINE_SPACING;
    return lv;
}

static void drawListBody() {
    ui_clear();
    ui_draw_text_content(4, g_font.ascent(), "Flomo", false);
    {
        std::string t = std::to_string((int)g.filtered.size()) + " 条";
        if (!g.query.empty()) t = "/" + g.query + "  " + t;
        else if (!g.tag.empty()) t = "#" + g.tag + "  " + t;
        int w = g_font.textWidth(t.c_str());
        ui_draw_text_content(SCREEN_W - w - 4, g_font.ascent(), t.c_str());
    }
    int sepY = FONT_H + 4;
    u8g2_DrawHLine(g_u8g2, 0, sepY, SCREEN_W);

    int y = sepY + LINE_SPACING;
    ListView lv = flomoListView(false);
    listViewFollow(lv);
    g.sel = lv.sel;
    g.scroll = lv.first;
    const int vis = lv.rows;

    for (int i = 0; i < vis && g.scroll + i < (int)g.filtered.size(); i++) {
        int fi = g.scroll + i;
        const Memo &m = g.store.memos[g.filtered[fi]];
        std::string date = m.updatedAt.size() >= 10 ? m.updatedAt.substr(5, 5) : "--";
        std::string line = (m.dirty ? "*" : " ") + date + "  " + firstLine(m.contentText);
        bool s = (fi == g.sel);
        ui_draw_text_content(8, y + i * LINE_SPACING, line.c_str(), s);
    }
    if (g.filtered.empty()) {
        const char *msg = !g.query.empty() ? "无匹配"
                          : (g.store.memos.empty() ? "暂无笔记 — 按 N 新建" : "无可见笔记");
        ui_draw_text_content(8, y, msg);
    }

    // 浮动按钮最后画，天然压在列表行上面。
    drawSyncFab();
    drawNewFab();
    ui_draw_status(listStatusLeft().c_str(), "");
}

static void drawList() {
    drawListBody();
    ui_commit();
}

// 反查鼠标点在哪一行（行高 = LINE_SPACING，首行基线 y）。
static int listRowAtY(int ty) {
    ListView lv = flomoListView(g.mode == FM_SEARCH);
    listViewFollow(lv);
    return listViewHitAt(lv, ty);
}

// 详情的画面本体（不含提交）：确认框 / 长按菜单要盖在它上面。
static void drawDetailBody() {
    ui_clear();
    if (g.filtered.empty()) { drawListBody(); return; }
    const Memo &m = g.store.memos[g.filtered[g.sel]];

    ui_draw_text_content(4, g_font.ascent(), m.updatedAt.c_str(), false);
    {
        char buf[32];
        snprintf(buf, sizeof(buf), "%d/%d", g.sel + 1, (int)g.filtered.size());
        int w = g_font.textWidth(buf);
        ui_draw_text_content(SCREEN_W - w - 4, g_font.ascent(), buf);
    }
    int sepY = FONT_H + 4;
    u8g2_DrawHLine(g_u8g2, 0, sepY, SCREEN_W);

    // 折行结果按「正文 + 折行宽度 + 当前字体」缓存。详情页每一拍都重画（滚一行、按一下键、
    // 转屏…），而同一篇笔记同一宽度下的折行结果当然一样 —— 以前每帧重新折一遍整篇，
    // wrapText 是逐字符推进、每一步都 para.substr() 新分配一个 std::string（改不了它，
    // 那是它量宽的方式），主机上量过：4KB 笔记一帧 1600 多次堆分配 / 54KB 拷贝。
    // 命中缓存时只比一次字符串（O(n) 比较、零分配），比重新折便宜一个量级。
    static std::string s_wrapSrc;
    static int s_wrapW = -1;
    static int s_wrapPx = -1;
    static std::string s_wrapFont;
    static std::vector<std::string> s_wrapLines;
    const int wrapW = SCREEN_W - 16;
    // 键还得带上**当前字体**：wrapText 逐字符量宽（g_font.textWidth），而量宽由共享格子的
    // px 高 + 用户选的外置字体面（拉丁步进）共同决定。只认「正文 + 宽度」的话，在设置里
    // 换过字号/字体再回到同一篇笔记，会拿旧折行结果去画 → 该折的行溢出、右边被切。
    // （CJK 步进恒等于格子 px，不受字体面影响；受影响的是中英混排里的拉丁。）
    const char *fp = font_store_get_path();
    const std::string fontPath = fp ? fp : "";
    if (s_wrapW != wrapW || s_wrapPx != g_font.pxHeight() || s_wrapFont != fontPath ||
        s_wrapSrc != m.contentText) {
        s_wrapSrc = m.contentText;
        s_wrapW = wrapW;
        s_wrapPx = g_font.pxHeight();
        s_wrapFont = fontPath;
        s_wrapLines = wrapText(s_wrapSrc, wrapW);
    }
    const std::vector<std::string> &lines = s_wrapLines;
    int top = sepY + LINE_SPACING;
    // 右下角那列浮动按钮占掉的竖向空间要扣掉：正文不往按钮底下铺，否则末尾几行
    // 的右端会被不透明的按钮压住。按钮列锚在 STATUS_BAR_Y 上（不是 STATUS_Y，
    // 两者差 4px），所以这里的下界也取 STATUS_BAR_Y，与 flomoFabY() 对得上。
    // （列表页是一行短标题，那边就不预留了。）
    int vis = (STATUS_BAR_Y - flomoFabColH() - top) / LINE_SPACING;
    if (vis < 1) vis = 1;
    int maxScroll = (int)lines.size() - vis;
    if (maxScroll < 0) maxScroll = 0;
    if (g.detailScroll > maxScroll) g.detailScroll = maxScroll;
    if (g.detailScroll < 0) g.detailScroll = 0;

    for (int i = 0; i < vis && g.detailScroll + i < (int)lines.size(); i++) {
        ui_draw_text_content(8, top + i * LINE_SPACING, lines[g.detailScroll + i].c_str());
    }
    if (lines.empty()) ui_draw_text_content(8, top, "（空）");

    drawEditFab();
    drawTrashFab(1);
    ui_draw_status(m.dirty ? "未同步·E编辑 D删除 Esc返回" : "E编辑 D删除 Esc返回", "");
}

static void drawDetail() {
    drawDetailBody();
    ui_commit();
}

static void drawConfirmDelete() {
    drawDetailBody();
    ui_draw_confirm_dialog("确认删除这条笔记？", "Enter确认  Esc取消", "");
    ui_commit();
}

// ── 长按一条笔记弹的菜单 ──────────────────────────────────────────────────
// 照 GTD 的 M_ITEM_MENU 那一套：居中方框的几何被**绘制与命中共用**；动作表只管
// "选中之后干什么"；执行时把选项映射回已有的键盘动作，菜单本身不含业务逻辑。

enum FlomoMenuAct { FMA_EDIT, FMA_COPY, FMA_DELETE, FMA_CANCEL };
static const FlomoMenuAct kFlomoMenuActs[] = {FMA_EDIT, FMA_COPY, FMA_DELETE, FMA_CANCEL};
static const int FLOMO_MENU_N = sizeof(kFlomoMenuActs) / sizeof(kFlomoMenuActs[0]);

static const char *flomoMenuLabel(FlomoMenuAct a) {
    switch (a) {
    case FMA_EDIT:   return "编辑";
    case FMA_COPY:   return "复制内容";
    case FMA_DELETE: return "删除";
    default:         return "取消";
    }
}

static void flomoMenuBoxRect(int *bx, int *by, int *bw, int *bh) {
    int w = 0;
    for (int i = 0; i < FLOMO_MENU_N; i++) {
        int tw = g_font.textWidth(flomoMenuLabel(kFlomoMenuActs[i])) + 2 * FONT_H;
        if (tw > w) w = tw;
    }
    if (w < 160) w = 160;
    if (w > SCREEN_W - 32) w = SCREEN_W - 32;
    *bw = w;
    *bh = FLOMO_MENU_N * LINE_SPACING + 16;
    *bx = (SCREEN_W - w) / 2;
    *by = (SCREEN_H - *bh) / 2;
}

// 每一项文字的基线（相对浮层顶）：绘制与命中共用同一个式子。
static int flomoMenuRowY(int by, int i) { return by + 8 + g_font.ascent() + i * LINE_SPACING; }

// 点按落在第几项；-1 = 落在框外（= 关闭菜单）。
static int flomoMenuHitAt(int x, int y) {
    int bx, by, bw, bh;
    flomoMenuBoxRect(&bx, &by, &bw, &bh);
    if (x < bx || x >= bx + bw || y < by || y >= by + bh) return -1;
    int i = (y - by - 8) / LINE_SPACING;
    if (i < 0) i = 0;
    if (i >= FLOMO_MENU_N) i = FLOMO_MENU_N - 1;
    return i;
}

// 列表 + 居中浮层，**一次提交**（浮层要赶在 ui_commit 之前画进同一块缓冲；
// 提交完再往那块缓冲上画就是跟 core1 抢同一帧了）。
static void drawItemMenu() {
    drawListBody();

    int bx, by, bw, bh;
    flomoMenuBoxRect(&bx, &by, &bw, &bh);
    u8g2_SetDrawColor(g_u8g2, 1);
    u8g2_DrawBox(g_u8g2, bx, by, bw, bh);
    u8g2_SetDrawColor(g_u8g2, 0);
    u8g2_DrawFrame(g_u8g2, bx, by, bw, bh);

    for (int i = 0; i < FLOMO_MENU_N; i++) {
        const char *lb = flomoMenuLabel(kFlomoMenuActs[i]);
        int ty = flomoMenuRowY(by, i);
        int tx = bx + (bw - g_font.textWidth(lb)) / 2;
        if (i == g.menuSel) {
            // 选中行反白：填黑底 + drawText(...,true) 画白字。
            // **不能**靠 u8g2_SetDrawColor 反色 —— TTF 渲染器完全不吃那个。
            u8g2_SetDrawColor(g_u8g2, 0);
            u8g2_DrawBox(g_u8g2, bx + 4, ty - g_font.ascent() - 2, bw - 8, FONT_H + 4);
            u8g2_SetDrawColor(g_u8g2, 1);
            g_font.drawText(tx, ty, lb, true);
            u8g2_SetDrawColor(g_u8g2, 0);
        } else {
            g_font.drawText(tx, ty, lb, false);
        }
    }

    ui_commit();
}

// 检索框里文本的左端 / 基线：绘制与触摸命中共用。
static int flomoSearchX() { return g_font.textWidth("检索:") + 8; }
static int flomoSearchY() { return g_font.ascent(); }

// 触摸选区的反白 + 按钮条（共享件 text_sel）；会话没开就是空操作。
// 检索框是**单行**（整块缓冲画在一行上），行表因此只有一行。
static void flomoSearchOverlay() {
    if (!textSelActive()) return;
    TextSelLine ln;
    ln.start = 0;
    ln.end = (int)g.searchBuf.size();
    ln.x0 = flomoSearchX();
    ln.baseline = flomoSearchY();
    TextSelView view{&ln, 1, editorVkVisible() ? editorVkTop() : SCREEN_H};
    textSelDraw(g.searchBuf, view);
}

static void drawSearch() {
    ui_clear();
    ui_draw_text_content(4, g_font.ascent(), "检索:", false);
    std::string display = g.searchBuf.empty() ? " " : g.searchBuf;
    int inputX = flomoSearchX();
    int inputY = flomoSearchY();
    g_content_font.drawText(inputX, inputY, display.c_str());
    int cx = inputX + g_font.textWidth(g.searchBuf.substr(0, g.searchCur).c_str());
    u8g2_SetDrawColor(g_u8g2, 0);
    u8g2_DrawFrame(g_u8g2, cx, inputY + 4, 2, 3);
    u8g2_SetDrawColor(g_u8g2, 1);

    int sepY = FONT_H + 4;
    u8g2_DrawHLine(g_u8g2, 0, sepY, SCREEN_W);

    const bool vk = editorVkVisible();
    bool composing = g.searchIme && g_ime.composing() && !vk;
    int listY = sepY + LINE_SPACING;
    ListView lv = flomoListView(true);
    listViewFollow(lv);
    g.sel = lv.sel;
    g.scroll = lv.first;
    const int vis = lv.rows;

    for (int i = 0; i < vis && g.scroll + i < (int)g.filtered.size(); i++) {
        const Memo &m = g.store.memos[g.filtered[g.scroll + i]];
        std::string date = m.updatedAt.size() >= 10 ? m.updatedAt.substr(5, 5) : "--";
        std::string line = date + "  " + firstLine(m.contentText);
        ui_draw_text_content(8, listY + i * LINE_SPACING, line.c_str(), g.scroll + i == g.sel);
    }
    if (g.filtered.empty())
        ui_draw_text_content(8, listY, g.searchBuf.empty() ? "输入关键词检索" : "无匹配");

    if (vk) editorVkDraw();
    else if (composing) drawIMEUIFullscreen();
    else ui_draw_status("Enter打开 Esc返回", "");
    flomoSearchOverlay();
    ui_commit();
}

static const char *HELP_LINES[] = {
    "── Flomo 笔记 ──",
    "N     新建笔记",
    "Enter 查看 / 编辑",
    "E     编辑当前笔记",
    "D     删除当前笔记",
    "R     手动同步（连 WiFi）",
    "/     搜索",
    "Esc   返回（列表里横划也返回）",
    "",
    "触摸：右下角 + 新建、云同步；",
    "详情页右下角 = 编辑 / 删除；",
    "长按一条笔记 = 弹出菜单。",
    "",
    "同步说明：新增/修改只存本地，",
    "带 * 的条目表示还没同步；",
    "按 R 或右下角云图标才会上传。",
};
static const int HELP_LINE_COUNT = sizeof(HELP_LINES) / sizeof(HELP_LINES[0]);

static void drawHelp() {
    ui_clear();
    ui_draw_text_content_centered(ui_title_baseline(), "Flomo 帮助", false);
    u8g2_DrawHLine(g_u8g2, 0, ui_title_baseline() + g_font.descent() + 4, SCREEN_W);
    int contentY = ui_title_baseline() + g_font.descent() + 12;
    int maxVis = (STATUS_Y - contentY) / LINE_SPACING;
    if (maxVis < 1) maxVis = 1;
    int maxScroll = HELP_LINE_COUNT - maxVis;
    if (maxScroll < 0) maxScroll = 0;
    if (g.helpScroll > maxScroll) g.helpScroll = maxScroll;
    if (g.helpScroll < 0) g.helpScroll = 0;
    for (int i = 0; i < maxVis && g.helpScroll + i < HELP_LINE_COUNT; i++) {
        const char *line = HELP_LINES[g.helpScroll + i];
        // 以前这里判了个"是否小节标题"（行首 U+E2xx 图标字形）当 bold 传下去，但那个
        // 形参从没被用过；小节之间的区分本来就是行首图标给的，判定删掉。
        ui_draw_text_content(12, contentY + i * LINE_SPACING + g_font.ascent(), line, false);
    }
    ui_draw_status("Esc返回", "");
    ui_commit();
}

// ── 借写作编辑器改正文 ────────────────────────────────────────────────────
// 正文交给 screen_editor，回程按灵感面板那套：编辑器把正文写进 journal 目录下的
// 临时文件，这里读回来落库，再删掉临时文件。
static AppState openEditorFor(ScreenContext &ctx, const std::string &slug, const std::string &content) {
    char name[48];
    snprintf(name, sizeof(name), "__flomo_%d_%lld", g.editorSeq++, (long long)std::time(nullptr));
    g.pendingTemp = name;
    g.pendingSlug = slug;

    ctx.editContent = content;
    ctx.editFilename = name;
    ctx.promptText.clear();          // 非空 = 提示写作，正文上方会多一条表头
    ctx.editorTitle = "Flomo";
    ctx.prevState = APP_FLOMO;
    ctx.nextState = APP_EDITOR;
    app_editor_request_reinit();
    return APP_EDITOR;
}

// ── 初始化 / 事件 ─────────────────────────────────────────────────────────

// 「从编辑器回来」那一拍：把临时文件里的正文落进库（新建一条，或改一条），返回是否
// 真落了东西。抽出来是因为它有两个调用点 —— 进屏那一刻（screen_flomo_init），以及
// 后台同步收工那一拍：同步在跑的时候进屏这一拍不能碰库（见 init 开头那段），编辑
// 结果得攒到 worker 收工之后再补。
static bool applyPendingEdit() {
    if (g.pendingTemp.empty()) return false;

    std::string path = std::string("/sdcard/pjournal/") + g.pendingTemp;
    std::string content = readTempFile(path);
    remove(path.c_str());
    std::string slug = g.pendingSlug;
    g.pendingTemp.clear();
    g.pendingSlug.clear();

    if (content.empty()) return false;
    std::string body = extractBody(content);
    if (body.empty()) body = content;
    while (!body.empty() && (body.back() == '\n' || body.back() == '\r')) body.pop_back();
    if (body.empty()) return false;

    if (slug.empty()) {
        // 新建
        Memo m;
        m.slug = "local-" + std::to_string((long long)std::time(nullptr));
        m.createdAt = nowStamp();
        m.updatedAt = m.createdAt;
        m.contentText = body;
        m.contentHtml = textToHtml(body);
        m.tags = extractTags(body);
        m.dirty = true;
        m.pendingOp = "create";
        g.store.memos.push_back(m);
        sortMemos(g.store.memos);
        g.status = "已保存到本地（按 R 同步）";
    } else {
        for (auto &m : g.store.memos) {
            if (m.slug != slug) continue;
            m.contentText = body;
            m.contentHtml = textToHtml(body);
            m.tags = extractTags(body);
            m.updatedAt = nowStamp();
            m.dirty = true;
            m.pendingOp = isLocalOnly(m) ? "create" : "update";
            g.status = "已保存到本地（按 R 同步）";
            break;
        }
        sortMemos(g.store.memos);
    }
    saveLocal();
    rebuildFilter();
    return true;
}

void screen_flomo_init(AppState returnTo) {
    g.returnTo = returnTo;
    g.drawn = false;   // 进屏第一拍（或从编辑器回来那一拍）必须画一次，见下面的空转记账

    // 后台同步正在跑（Esc 走开、又回来了）：worker 手里那份库快照是它开始那一刻的，
    // 这会儿读库/写库都会和它抢 —— 它最后整库落盘，谁后写谁赢，另一边白干。所以这
    // 一拍什么都不碰：不 load、不落编辑结果，全攒到它收工那一拍再补（见 handle 里的
    // FSYNC_DONE 分支）。这期间屏上是上一次读进来的旧库，符合预期。
    if (s_syncState.load(std::memory_order_acquire) == FSYNC_RUNNING) return;

    loadLocal();

    // 从编辑器回来：读临时文件、落库，并保留列表位置。
    if (!g.pendingTemp.empty()) {
        applyPendingEdit();
        return;
    }

    g.sel = 0;
    g.scroll = 0;
    rebuildFilter();
}

AppState screen_flomo_handle(int key, ScreenContext &ctx) {
    // ── 后台同步的两个状态（见上面「同步：跑在后台任务里」那段）──────────
    const int syncNow = s_syncState.load(std::memory_order_acquire);
    if (syncNow == FSYNC_RUNNING) {
        // Esc 放行：worker 写的是磁盘，不依赖这一屏 —— 走开。回来（或下次进 flomo
        // loadLocal）看到的照样是新的库。原来这条路走不掉：同步整个卡在主任务里，
        // 键根本轮不到处理。
        if (key == 0x1B) { ctx.nextState = g.returnTo; return g.returnTo; }
        drawSyncShared();
        return APP_FLOMO;
    }
    if (syncNow == FSYNC_DONE) {   // worker 收工：把库和结果收进来
        s_syncState.store(FSYNC_IDLE, std::memory_order_release);
        syncLock();
        const std::string res = s_syncResult;
        const bool ok = s_syncOk;
        s_syncResult.clear();
        syncUnlock();

        loadLocal();   // worker 只碰了磁盘（MemoDb::save 落盘后才置 DONE），这里读回来
        // 同步期间在编辑器里写的那条（进屏那一拍被攒下了，见 screen_flomo_init）：
        // 现在库安静了，补进这份刚读回来的库。
        if (!g.pendingTemp.empty()) applyPendingEdit();
        rebuildFilter();
        // 库可能比进来时短（远端删过、或这一趟真删了）：高亮下标夹回范围内，否则
        // 下面各处的 g.filtered[g.sel] 会越界读。原来在主任务里跑时靠"返回前重建索引"
        // 躲过这一条，现在重建搬到这一拍，夹一下更稳。
        if (g.sel >= (int)g.filtered.size()) g.sel = (int)g.filtered.size() - 1;
        if (g.sel < 0) g.sel = 0;
        if (ok) {
            char buf[64];
            snprintf(buf, sizeof(buf), "同步完成 %d 条", (int)g.filtered.size());
            g.status = buf;
        } else {
            g.status = res.empty() ? "同步失败" : res;
        }
        g.drawn = false;    // 内容换了：这一拍照画（见下面空转记账）
    }

    // ── 空转不重画 ────────────────────────────────────────────────────────
    // 本屏每一处状态变化都发生在**按键那一拍**，而且那一拍自己就画了（下面每个分支
    // 末尾都有 drawXxx + return），所以空转这一趟要画的永远和上一幅一模一样。以前它
    // 每 100ms（idle_ms 默认值）重画一次整屏再 ui_commit：白烧一遍绘制 + 一次全帧
    // 差分扫描，详情页还要把整篇笔记重新折行（见 drawDetailBody）。更要紧的是每次
    // commit 都要占一块帧缓冲（就 2 块），而渲染任务那边一次整屏刷要 400 多毫秒 ——
    // 攒起来主循环就会卡在 acquire_buffer 里，卡住期间触摸根本没被轮询，屏上表现
    // 就是"点哪条笔记都没反应"。
    //
    // 两个例外照画：① 屏幕几何变了（自适应转屏把方向转过去了，屏宽高一变这幅画就
    // 作废，和编辑器那边 screen_editor_reset_drawn 是同一件事，只是这边自己看得出来，
    // 不必让主循环知道 flomo）；② 浮标还亮着或刚灭（要把它画出来 / 擦掉）。
    // 进屏、从编辑器回来那一拍由 screen_flomo_init 把 drawn 清掉，照画。
    const bool toastNow = ui_toast_active();
    if (key == 0 && g.drawn && g.drawnW == SCREEN_W && g.drawnH == SCREEN_H &&
        !toastNow && !g.toastWas) {
        return APP_FLOMO;
    }
    g.drawn = true;
    g.drawnW = SCREEN_W;
    g.drawnH = SCREEN_H;
    g.toastWas = toastNow;

    // 触摸上下滑的翻页键（主循环不再替写作界面回退成单步）：**列表和长文**按屏翻
    // ——下面的 FM_LIST / FM_SEARCH / FM_DETAIL / FM_HELP 各自接 KEY_PAGE_*；
    // 其余子状态（删除确认、条目菜单）保持原来的单步语义。
    if (key == KEY_PAGE_UP || key == KEY_PAGE_DOWN) {
        const bool pageMode = (g.mode == FM_LIST || g.mode == FM_SEARCH ||
                               g.mode == FM_DETAIL || g.mode == FM_HELP);
        if (!pageMode) key = (key == KEY_PAGE_UP) ? KEY_UP : KEY_DOWN;
    }

    // 换了子状态 = 换了字段：上一个检索框的选区 / 按钮条作废。
    {
        static int s_prevFmMode = -1;
        if ((int)g.mode != s_prevFmMode) { textSelReset(); s_prevFmMode = (int)g.mode; }
    }

    switch (g.mode) {
    // ── 帮助 ──
    case FM_HELP:
        if (key == 0x1B || key == 'q' || key == 'Q' || key == 0x0A || key == 0x0D) g.mode = FM_LIST;
        else if (key == KEY_UP && g.helpScroll > 0) g.helpScroll--;
        else if (key == KEY_DOWN) g.helpScroll++;
        else if (key == KEY_PAGE_UP) { g.helpScroll -= flomoHelpVis(); if (g.helpScroll < 0) g.helpScroll = 0; }
        else if (key == KEY_PAGE_DOWN) g.helpScroll += flomoHelpVis();  // 上限在画的时候夹住
        drawHelp();
        return APP_FLOMO;

    // ── 删除确认 ──
    case FM_CONFIRM_DELETE:
        if (key == 0x0A || key == 0x0D || key == 'y' || key == 'Y') {
            if (!g.filtered.empty()) {
                int idx = g.filtered[g.sel];
                Memo &m = g.store.memos[idx];
                if (isLocalOnly(m) && !m.dirty) {
                    g.store.memos.erase(g.store.memos.begin() + idx);
                } else if (isLocalOnly(m)) {
                    // 还没上传过的新笔记，直接丢掉，不用给服务端发删除。
                    g.store.memos.erase(g.store.memos.begin() + idx);
                } else {
                    m.deleted = true;
                    m.dirty = true;
                    m.pendingOp = "delete";
                }
                saveLocal();
                rebuildFilter();
                g.status = "已删除（按 R 同步）";
            }
            g.mode = FM_LIST;
        } else if (key == 0x1B || key == 'n' || key == 'N') {
            g.mode = FM_LIST;
        }
        drawConfirmDelete();
        return APP_FLOMO;

    // ── 搜索 ──
    case FM_SEARCH: {
        {   // 虚拟键盘点按 → 键码
            int vkKey = 0;
            bool turnOn = false;
            if (editorVkPumpTap(g.searchIme, &vkKey, &turnOn)) {
                if (turnOn) { g.searchIme = true; g_ime.setActive(true); }
                key = vkKey;
            }
        }
        // 触摸编辑（共享件 text_sel）：长按检索行 → 复制/剪切/粘贴/全选。
        if (key == KEY_TOUCH_LONG || textSelActive()) {
            int tx = 0, ty = 0;
            const bool hasTap = input_tap_xy(&tx, &ty);
            TextSelLine fln;
            fln.start = 0;
            fln.end = (int)g.searchBuf.size();
            fln.x0 = flomoSearchX();
            fln.baseline = flomoSearchY();
            TextSelView fview{&fln, 1, editorVkVisible() ? editorVkTop() : SCREEN_H};
            if (key == KEY_TOUCH_LONG) {
                if (hasTap && textSelBegin(g.searchBuf, g.searchCur, fview, tx, ty)) {
                    g_ime.cancelComposition();
                    drawSearch();
                    return APP_FLOMO;
                }
                // 没落在检索行上：落在某条**结果**行上就弹同一个笔记菜单，
                // 否则才交给下面既有的"长按 = 返回"分支。
                // （下半屏才算结果区——否则点检索行右端的空白会被当成本页第一行。）
                if (hasTap && ty >= FONT_H + 4 + LINE_SPACING / 2) {
                    int rfi = listRowAtY(ty);
                    if (rfi >= 0) {
                        g.searchIme = false;
                        g_ime.setActive(false);
                        editorVkAutoHide();
                        g.sel = rfi;
                        g.menuSel = 0;
                        g.mode = FM_ITEM_MENU;
                        drawItemMenu();
                        return APP_FLOMO;
                    }
                }
                key = 0x1B;   // 没落在检索行上 → 交给下面既有的"长按 = 返回"分支
            } else if (key != 0 &&
                       textSelHandleKey(g.searchBuf, g.searchCur, fview, key, tx, ty, hasTap,
                                        &ctx.statusMessage)) {
                g.query = g.searchBuf;
                rebuildFilter();
                g.sel = 0; g.scroll = 0;
                drawSearch();
                return APP_FLOMO;
            }
        }
        if (g.searchIme && key != 0) {
            std::string imeOut;
            if (g_ime.handleKey(key, imeOut)) {
                if (!imeOut.empty()) {
                    imeFieldInsert(flomoSearchField(), imeOut);
                    g.query = g.searchBuf;
                    rebuildFilter();
                    g.sel = 0; g.scroll = 0;
                }
                drawSearch();
                return APP_FLOMO;
            }
        }
        if (key == KEY_IME_TOGGLE) {
            g.searchIme = !g.searchIme;
            g_ime.setActive(g.searchIme);
            drawSearch();
            return APP_FLOMO;
        }
        if (key == KEY_FULLWIDTH_TOGGLE) { g_ime.toggleFullwidth(); drawSearch(); return APP_FLOMO; }
        if (key == 0x1B) {
            g.searchIme = false;
            g_ime.setActive(false);
            g.mode = FM_LIST;
            editorVkAutoHide();
        } else if (key == 0x0A || key == 0x0D) {
            g.searchIme = false;
            g_ime.setActive(false);
            g.mode = FM_DETAIL;
            g.detailScroll = 0;
            editorVkAutoHide();
        } else if (key == KEY_UP || key == KEY_DOWN || key == KEY_PAGE_UP || key == KEY_PAGE_DOWN ||
                   key == KEY_HOME || key == KEY_END) {
            ListView lv = flomoListView(true);
            if (listViewKey(lv, key)) {
                listViewFollow(lv);
                g.sel = lv.sel;
                g.scroll = lv.first;
            }
        } else if (key == KEY_TOUCH_LONG) {
            g.searchIme = false;
            g_ime.setActive(false);
            g.mode = FM_LIST;
            editorVkAutoHide();
        } else if (key == 0x7F || key == 0x08) {
            if (imeFieldBackspace(flomoSearchField())) {
                g.query = g.searchBuf;
                rebuildFilter();
                g.sel = 0; g.scroll = 0;
            }
        } else if (key >= 0x20 && key <= 0x7E) {
            imeFieldInsert(flomoSearchField(), std::string(1, (char)key));
            g.query = g.searchBuf;
            rebuildFilter();
            g.sel = 0; g.scroll = 0;
        } else if (key == '\n') {
            // 触摸抬手的回车：打开当前结果。
            g.searchIme = false;
            g_ime.setActive(false);
            g.mode = FM_DETAIL;
            g.detailScroll = 0;
            editorVkAutoHide();
        }
        drawSearch();
        return APP_FLOMO;
    }

    // ── 详情 ──
    case FM_DETAIL: {
        if (key == 0x1B || key == KEY_TOUCH_LONG) { g.mode = FM_LIST; drawList(); return APP_FLOMO; }
        if (key == 'e' || key == 'E') {
            if (!g.filtered.empty()) {
                const Memo &m = g.store.memos[g.filtered[g.sel]];
                return openEditorFor(ctx, m.slug, m.contentText);
            }
        }
        if (key == 'd' || key == 'D') { g.mode = FM_CONFIRM_DELETE; drawConfirmDelete(); return APP_FLOMO; }
        if (key == KEY_UP || key == KEY_DOWN || key == KEY_PAGE_UP || key == KEY_PAGE_DOWN) {
            scrollTextKey(key, g.detailScroll, flomoDetailVis(), 100000);
        }
        else if (key == '\n') {
            // 触摸点按：右下角浮动按钮（编辑 / 删除）优先，点别处 = 返回列表
            // （笔记长时用上下键滚）。
            int tx = 0, ty = 0;
            if (input_tap_xy(&tx, &ty)) {
                if (!g.filtered.empty()) {
                    const Memo &m = g.store.memos[g.filtered[g.sel]];
                    if (flomoFabHit(0, tx, ty)) return openEditorFor(ctx, m.slug, m.contentText);
                    if (flomoFabHit(1, tx, ty)) {
                        g.mode = FM_CONFIRM_DELETE;
                        drawConfirmDelete();
                        return APP_FLOMO;
                    }
                }
                g.mode = FM_LIST;
                drawList();
                return APP_FLOMO;
            }
        }
        drawDetail();
        return APP_FLOMO;
    }

    // ── 长按弹出的笔记菜单 ──
    case FM_ITEM_MENU: {
        // 触摸：落在框内某行 = 选中它再当 Enter 执行；落在框外 = 关闭。
        if (key == '\n') {
            int tx = 0, ty = 0;
            if (input_tap_xy(&tx, &ty)) {   // 键盘 Enter 没有待取的点按，不会进这里
                int hit = flomoMenuHitAt(tx, ty);
                if (hit < 0) { g.mode = FM_LIST; drawList(); return APP_FLOMO; }
                g.menuSel = hit;
            }
            key = 0x0A;
        }
        if (key == KEY_UP || key == 'k') {
            if (g.menuSel > 0) g.menuSel--;
        } else if (key == KEY_DOWN || key == 'j') {
            if (g.menuSel < FLOMO_MENU_N - 1) g.menuSel++;
        } else if (key == 0x0A || key == 0x0D) {
            FlomoMenuAct a = (g.menuSel >= 0 && g.menuSel < FLOMO_MENU_N)
                                 ? kFlomoMenuActs[g.menuSel] : FMA_CANCEL;
            g.mode = FM_LIST;   // 无论选哪项都先退出浮层（也覆盖去编辑器再回来的情形）
            if (!g.filtered.empty()) {
                const Memo &m = g.store.memos[g.filtered[g.sel]];
                if (a == FMA_EDIT) return openEditorFor(ctx, m.slug, m.contentText);
                if (a == FMA_COPY) {
                    clipboardPush(m.contentText);
                    g.status = clipboardLastTruncated() ? "已复制到粘贴板（超长已截断）"
                                                        : "已复制到粘贴板";
                } else if (a == FMA_DELETE) {
                    g.mode = FM_CONFIRM_DELETE;   // 复用现有确认框与删除逻辑
                    drawConfirmDelete();
                    return APP_FLOMO;
                }
            }
            drawList();
            return APP_FLOMO;
        } else if (key == 0x1B || key == 'q' || key == 'Q') {
            g.mode = FM_LIST;
            drawList();     // 别落到下面的 drawItemMenu（那样会多画一帧浮层）
            return APP_FLOMO;
        }
        drawItemMenu();
        return APP_FLOMO;
    }

    // ── 列表 ──
    default: break;
    }

    // 触摸长按：列表行上 = 弹出笔记菜单（编辑/复制/删除）
    if (key == KEY_TOUCH_LONG) {
        int tx = 0, ty = 0;
        if (input_tap_xy(&tx, &ty)) {
            // 长按落在浮动按钮上：什么都不做。少了这个守卫，"长按按钮"会落进
            // 下面那句"长按空白 = 退出界面"（蓝牙管理页有同样的守卫）。
            if (flomoFabHit(0, tx, ty) || flomoFabHit(1, tx, ty)) return APP_FLOMO;
            int fi = listRowAtY(ty);
            if (fi >= 0) {
                g.sel = fi;
                g.menuSel = 0;
                g.mode = FM_ITEM_MENU;
                drawItemMenu();
                return APP_FLOMO;
            }
        }
        // 长按在空白处 = 返回
        ctx.nextState = g.returnTo;
        return g.returnTo;
    }

    // 触摸点按
    if (key == '\n') {
        int tx = 0, ty = 0;
        if (input_tap_xy(&tx, &ty)) {
            // 浮动按钮优先于行命中：顺序即优先级。
            if (flomoFabHit(1, tx, ty)) return openEditorFor(ctx, "", "");   // ＋ 新建
            if (flomoFabHit(0, tx, ty)) {                                    // 云 同步
                g.mode = FM_LIST;
                flomoTriggerSync();
                drawSyncShared();   // 立刻把进度画出来，别先在列表上闪一下
                return APP_FLOMO;
            }
            int fi = listRowAtY(ty);
            if (fi >= 0) {
                g.sel = fi;
                g.detailScroll = 0;
                g.mode = FM_DETAIL;
                drawDetail();
                return APP_FLOMO;
            }
        }
        // 空白处点按 = 打开当前选中项
        if (!g.filtered.empty()) {
            g.detailScroll = 0;
            g.mode = FM_DETAIL;
            drawDetail();
            return APP_FLOMO;
        }
        drawList();
        return APP_FLOMO;
    }

    // 划动返回（列表页）：手指在**屏幕中间**横划一下 = 退出到写作模式（g.returnTo，
    // 与 Esc 同一个去处）。从屏边 80px 带里起划的那种不走这里 —— hw/input 直接发
    // KEY_BACK，主循环把它翻成 0x1B，落进下面那条 Esc 分支，两条路同一结果。
    //
    // 以前这一划是"什么都不做"：横划抬手只产生 KEY_LEFT/KEY_RIGHT，本函数没人接，
    // 一路落到末尾的 drawList() 白重画一帧。
    //
    // 用按下点把**触摸划动**和蓝牙键盘的 ←/→ 分开：只有触摸手势的按键带按下点
    // （hw/input 的 s_press_origin_valid 每帧开头就清，见 input_poll 首行那段注释
    // ——"本帧没有新手势抬手就不该有值"就是为了防这种误读）。实体键盘的方向键在
    // 列表里保持原来的"什么都不做"。方向不挑：左起划、右起划都算返回，和屏边
    // 那条（from_left / from_right 都发 KEY_BACK）一致。
    if ((key == KEY_LEFT || key == KEY_RIGHT) && input_press_xy(nullptr, nullptr)) {
        ctx.nextState = g.returnTo;
        return g.returnTo;
    }

    if (key == 0x1B || key == 'q' || key == 'Q') {
        ctx.nextState = g.returnTo;
        return g.returnTo;
    } else if (key == KEY_UP || key == KEY_DOWN || key == KEY_PAGE_UP || key == KEY_PAGE_DOWN ||
               key == KEY_HOME || key == KEY_END) {
        ListView lv = flomoListView(false);
        if (listViewKey(lv, key)) {
            listViewFollow(lv);
            g.sel = lv.sel;
            g.scroll = lv.first;
        }
        g.status.clear();
    } else if (key == 'n' || key == 'N') {
        return openEditorFor(ctx, "", "");
    } else if (key == 'e' || key == 'E') {
        if (!g.filtered.empty()) {
            const Memo &m = g.store.memos[g.filtered[g.sel]];
            return openEditorFor(ctx, m.slug, m.contentText);
        }
    } else if (key == 'd' || key == 'D') {
        if (!g.filtered.empty()) { g.mode = FM_CONFIRM_DELETE; drawConfirmDelete(); return APP_FLOMO; }
    } else if ((key == 0x0A || key == 0x0D) && !g.filtered.empty()) {
        g.detailScroll = 0;
        g.mode = FM_DETAIL;
        drawDetail();
        return APP_FLOMO;
    } else if (key == 'r' || key == 'R') {
        g.mode = FM_LIST;
        flomoTriggerSync();
        drawSyncShared();   // 同上：这一拍就出进度界面
        return APP_FLOMO;
    } else if (key == '/') {
        g.searchBuf = g.query;
        imeFieldMoveEnd(flomoSearchField());
        g.searchIme = true;
        g_ime.setActive(true);
        g.sel = 0;
        g.scroll = 0;
        g.mode = FM_SEARCH;
        editorVkAutoShow();   // 没连蓝牙键盘就弹虚拟键盘（有则内部不动）
        drawSearch();
        return APP_FLOMO;
    } else if (key == '?') {
        g.helpScroll = 0;
        g.mode = FM_HELP;
        drawHelp();
        return APP_FLOMO;
    }

    drawList();
    return APP_FLOMO;
}
