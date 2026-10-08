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
#include "icon_font.h"
#include "ime/IME.h"
#include "ui/ime_field.h"  // 输入框绑定：落串/退格/光标的 UTF-8 算术（与写作/计划/阅读共用一份）
#include "screen_editor.h"
#include "settings_manager.h"
#include "ui_helpers.h"
#include "ui/list_view.h"  // listPageStep：手写列表的翻页步长（一屏行数 / 没得翻 = 0）
#include "wifi_manager.h"
#include "hw/input.h"
#include "editor_vk.h"   // 虚拟键盘：检索框没连蓝牙键盘时的唯一输入途径
#include "text_sel.h"    // 单行输入框的触摸选字 / 粘贴板（三模式共享底层件）
#include "clipboard.h"   // 长按菜单的「复制内容」

#include <cstdio>
#include <cstring>
#include <ctime>
#include <algorithm>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
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

    // 同步态
    bool syncing = false;
    std::string syncMsg;

    // 编辑器交接：pendingTemp 是 journal 目录下的临时文件名，
    // pendingSlug 为空 = 新建，否则是正在编辑的那条笔记的 slug。
    std::string pendingTemp;
    std::string pendingSlug;
    int editorSeq = 0;

    // 搜索输入
    std::string searchBuf;
    int searchCur = 0;
    bool searchIme = false;
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

// ── 同步 ──────────────────────────────────────────────────────────────────

// 同步界面（居中一行提示），syncProgress 每换一段就重画一次，让用户看到进度。
static void drawSync() {
    ui_clear();
    int base = (SCREEN_H - FONT_H) / 2;
    ui_draw_text_content_centered(base, "Flomo 同步", false);
    ui_draw_text_content_centered(base + FONT_H + 6, g.syncMsg.c_str());
    ui_draw_text_content_centered(STATUS_Y, "请稍候…");
    ui_commit();
}

static void syncProgress(const std::string &msg) {
    g.syncing = true;
    g.syncMsg = msg;
    drawSync();
}

// 连 WiFi，同时把"正在连接"报出来（ensure_wifi_connected() 不回调，自己写一遍）。
static bool ensureWifiProgress() {
    if (g_wifi.isConnected()) return true;
    std::string ssid = g_settings.wifiSsid();
    std::string pass = g_settings.wifiPassword();
    if (ssid.empty()) { g.status = "未配置 WiFi（先到设置填写）"; return false; }
    g_wifi.begin();
    if (!g_wifi.connect(ssid.c_str(), pass.c_str())) { g.status = "WiFi 连接失败"; return false; }
    for (int i = 0; i < 100; i++) {
        if (g_wifi.isConnected()) return true;
        static const char *dots[] = {"...", "....", ".....", ".."};
        syncProgress(std::string("正在连接 WiFi") + dots[i % 4]);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    g.status = "WiFi 连接超时";
    return false;
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

// 手动同步：先把本地 dirty 的改动推上去，再逐页拉取远端较新的笔记。
// 返回是否成功（失败时 g.status 里是原因）。
static bool flomoSync() {
    g.syncing = true;

    if (!ensureWifiProgress()) { g.syncing = false; return false; }

    std::string token = g_settings.flomoToken();
    if (token.empty()) {
        std::string email = g_settings.flomoEmail();
        std::string pass = g_settings.flomoPassword();
        if (email.empty() || pass.empty()) {
            g.syncing = false;
            g.status = "未配置 Flomo 账号（进设置填写）";
            return false;
        }
        syncProgress("正在登录 Flomo…");
        ApiResult r = FlomoApi::login(email, pass);
        if (!r.ok) { g.syncing = false; g.status = "登录失败: " + r.message; return false; }
        token = r.data["access_token"].asString();
        if (token.empty()) { g.syncing = false; g.status = "登录未返回令牌"; return false; }
        g_settings.setFlomoToken(token);
        g.store.token = token;
    }

    FlomoApi api(token);

    // ① 推送本地改动
    int dirtyTotal = 0;
    for (auto &m : g.store.memos) if (m.dirty) dirtyTotal++;
    int dirtyDone = 0;
    for (auto &m : g.store.memos) {
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
        syncProgress(prog);

        if (!r.ok) {
            saveLocal();
            g.syncing = false;
            g.status = opName + "失败: " + r.message;
            // 失败也要重建索引：这一趟可能已经替换过若干 memo（m = remote），
            // 索引表必须与 memos 对得上，否则调用方回来直接 drawList 会拿旧下标取元素。
            rebuildFilter();
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
    g.store.memos.erase(std::remove_if(g.store.memos.begin(), g.store.memos.end(),
                                       [](const Memo &m) { return m.deleted && !m.dirty; }),
                        g.store.memos.end());

    // ② 拉取远端（分页，最多 30 页兜底）
    std::string slug, updated;
    for (int page = 1; page <= 30; ++page) {
        char prog[48];
        snprintf(prog, sizeof(prog), "拉取笔记 第 %d 页…", page);
        syncProgress(prog);
        ApiResult r = api.listPage(slug, updated);
        if (!r.ok) {
            saveLocal();
            g.syncing = false;
            g.status = "刷新失败: " + r.message;
            // 关键：上面已经 erase 掉「已删且无标记」的本地条目、也可能 removeMemoBySlug
            // 过，memos 比进来时短了。不重建 g.filtered 就返回，调用方的 drawList 会拿
            // 旧下标去 memos[g.filtered[fi]] 越界读 → 崩。
            rebuildFilter();
            return false;
        }
        if (!r.data.isArray() || r.data.size() == 0) break;
        for (size_t i = 0; i < r.data.size(); ++i) {
            Memo m = memoFromJson(r.data[i]);
            if (m.deleted) removeMemoBySlug(g.store.memos, m.slug);
            else upsertMemo(g.store.memos, m);
            slug = m.slug;
            updated = m.updatedAt;
        }
        if (r.data.size() < 200) break;
    }

    g.store.lastSync = nowStamp();
    sortMemos(g.store.memos);
    saveLocal();
    rebuildFilter();

    char buf[64];
    snprintf(buf, sizeof(buf), "同步完成 %d 条", (int)g.filtered.size());
    g.status = buf;
    g.syncing = false;
    return true;
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
    int vis = (STATUS_Y - y + LINE_SPACING - 1) / LINE_SPACING;
    if (vis < 1) vis = 1;
    if (g.sel < g.scroll) g.scroll = g.sel;
    if (g.sel >= g.scroll + vis) g.scroll = g.sel - vis + 1;

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
    int y = FONT_H + 4 + LINE_SPACING;
    int i = (ty - y + LINE_SPACING / 2) / LINE_SPACING;
    if (i < 0) return -1;
    int fi = g.scroll + i;
    if (fi < 0 || fi >= (int)g.filtered.size()) return -1;
    return fi;
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

    std::vector<std::string> lines = wrapText(m.contentText, SCREEN_W - 16);
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
    // 列表底边：虚拟键盘弹着时裁到键盘面板顶边，否则照旧（候选条在时再让一让）。
    int listMaxY = vk ? (editorVkTop() - LINE_SPACING)
                      : (composing ? imeFullscreenPanelTopY() - LINE_SPACING : SCREEN_H);
    int vis = (listMaxY - listY + LINE_SPACING - 1) / LINE_SPACING;
    if (vis < 1) vis = 1;
    if (g.sel < g.scroll) g.scroll = g.sel;
    if (g.sel >= g.scroll + vis) g.scroll = g.sel - vis + 1;

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
    "Esc   返回",
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

void screen_flomo_init(AppState returnTo) {
    g.returnTo = returnTo;
    loadLocal();

    // 从编辑器回来：读临时文件，落库。
    if (!g.pendingTemp.empty()) {
        std::string path = std::string("/sdcard/pjournal/") + g.pendingTemp;
        std::string content = readTempFile(path);
        remove(path.c_str());
        std::string temp = g.pendingTemp;
        std::string slug = g.pendingSlug;
        g.pendingTemp.clear();
        g.pendingSlug.clear();

        if (!content.empty()) {
            std::string body = extractBody(content);
            if (body.empty()) body = content;
            while (!body.empty() && (body.back() == '\n' || body.back() == '\r')) body.pop_back();
            if (!body.empty()) {
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
            }
        }
        return;   // 保留列表位置
    }

    g.sel = 0;
    g.scroll = 0;
    rebuildFilter();
}

AppState screen_flomo_handle(int key, ScreenContext &ctx) {
    // 同步进行中是阻塞的（在 flomoSync 里跑完才回来），这里只是兜底。
    if (g.syncing) { drawSync(); return APP_FLOMO; }

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
        } else if (key == KEY_UP) {
            if (g.sel > 0) g.sel--;
        } else if (key == KEY_DOWN) {
            if (g.sel < (int)g.filtered.size() - 1) g.sel++;
        } else if (key == KEY_PAGE_UP || key == KEY_PAGE_DOWN) {
            // 整页翻：一步一屏，高亮跟着页走；一屏放得下就没得翻（吃掉这一划）。
            const int n = (int)g.filtered.size();
            const int step = listPageStep(n, flomoSearchVis());
            if (step > 0) {
                g.sel += (key == KEY_PAGE_DOWN) ? step : -step;
                if (g.sel < 0) g.sel = 0;
                if (g.sel > n - 1) g.sel = n - 1;
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
        if (key == KEY_UP) { if (g.detailScroll > 0) g.detailScroll--; }
        else if (key == KEY_DOWN) { g.detailScroll++; }
        else if (key == KEY_PAGE_UP) { g.detailScroll -= flomoDetailVis(); if (g.detailScroll < 0) g.detailScroll = 0; }
        else if (key == KEY_PAGE_DOWN) { g.detailScroll += flomoDetailVis(); }
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
                flomoSync();
                g.mode = FM_LIST;
                drawList();
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

    if (key == 0x1B || key == 'q' || key == 'Q') {
        ctx.nextState = g.returnTo;
        return g.returnTo;
    } else if (key == KEY_UP) {
        if (g.sel > 0) g.sel--;
        g.status.clear();
    } else if (key == KEY_DOWN) {
        if (g.sel < (int)g.filtered.size() - 1) g.sel++;
        g.status.clear();
    } else if (key == KEY_PAGE_UP || key == KEY_PAGE_DOWN) {
        // 整页翻：一步一屏，高亮跟着页走；一屏放得下就没得翻（吃掉这一划）。
        const int n = (int)g.filtered.size();
        const int step = listPageStep(n, flomoListVis());
        if (step > 0) {
            g.sel += (key == KEY_PAGE_DOWN) ? step : -step;
            if (g.sel < 0) g.sel = 0;
            if (g.sel > n - 1) g.sel = n - 1;
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
        flomoSync();
        g.mode = FM_LIST;
        drawList();
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
