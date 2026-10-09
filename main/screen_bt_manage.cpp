#include "screen_bt_manage.h"
#include "bt_keyboard.h"
#include "ui_helpers.h"
#include "ui/list_view.h"  // listPageStep：手写列表的翻页步长（一屏行数 / 没得翻 = 0）
#include "quick_edit.h"
#include "input.h"       // input_tap_xy：点按/长按的落点靠它（浮动按钮与长按菜单都要）
#include <cstdio>
#include <esp_timer.h>
#include "u8g2_shim.h"

// ── BT manage state ───────────────────────────────────────────────────────
enum BtMode { BT_MANAGE, BT_SCAN };

// 长按设备行弹出的操作菜单项。菜单只有三件事，与键盘上的 Enter/d 走同一条落点
// （btConnectPairedAt / btDeletePairedAt），不另开一套实现。
enum BtMenuAct { BT_MENU_CONNECT, BT_MENU_DELETE, BT_MENU_CANCEL };
static const char *BT_MENU_LABELS[] = {"连接", "删除设备", "取消"};
static const int BT_MENU_N = (int)(sizeof(BT_MENU_LABELS) / sizeof(BT_MENU_LABELS[0]));

static struct {
    BtMode mode = BT_MANAGE;    // 默认进入已配对设备管理
    int selection = 0;
    int scroll = 0;
    bool scanning = false;
    bool connecting = false;
    int64_t conn_start_ms = 0;
    char statusMsg[64];
    bool showHelp = false;
    int helpScroll = 0;
    // 长按设备行弹出的操作菜单（浮层）。menuTarget 是 g_bt 配对列表下标。
    bool menuOpen = false;
    int menuSel = 0;
    int menuTarget = -1;
} g_btState;

bool screen_bt_manage_scan_mode() { return g_btState.mode == BT_SCAN; }

// ── 几何：开关行 / 列表行 / 浮动按钮 / 长按菜单 ──────────────────────────
// 全局蓝牙开关那一行的基线：标题分隔线下面第一行，固定在列表**上面**。
// 做成独立一行而不是列表里的一项，是因为关掉蓝牙后列表必然是空的 —— 那一屏就只剩
// 这一件事可做，位置必须固定、显眼，也不能随列表滚动跑掉。
static int btSwitchRowY() { return FONT_H + 8 + LINE_SPACING; }
// 开关行的触摸命中带（文字基线上下各留一点，整行都给）。
static bool btSwitchRowHit(int y) {
    const int top = btSwitchRowY() - g_font.ascent() - 2;
    return y >= top && y < top + FONT_H + 4;
}
// 开/关药丸的几何（绘制与命中共用，虽然现在只有绘制用）。贴右边缘。
static void btSwitchPill(int *x, int *y, int *w, int *h) {
    *w = g_font.textWidth("开") + 24;
    *h = FONT_H + 4;
    *x = SCREEN_W - *w - 10;
    *y = btSwitchRowY() - g_font.ascent() - 2;
}

// 列表首行的**基线**。绘制与长按命中必须用同一个式子，否则"按哪行弹哪行"会错位。
static int manageRowBase() {
    int y = btSwitchRowY() + FONT_H;         // 开关行下面一行起
    if (g_btState.connecting) y += FONT_H;   // 连接中的状态行占掉一行，列表整体下移
    return y;
}

// 浮动按钮「+ 添加设备」：锚在状态栏上方右下角。做成浮动而不是列表最后一行，
// 是因为设备多了列表会一直排到状态栏——常驻一行按钮会跟列表抢位置。
static int fabW() { return g_font.textWidth("+ 添加设备") + 36; }
static int fabH() { return FONT_H + 10; }
static int fabX() { return SCREEN_W - fabW() - 10; }
static int fabY() { return STATUS_Y - fabH() - 8; }

// ── 翻页步长（触摸上下滑 = 整页翻）：与对应绘制函数**同一个基准**（那边是排版，
// 这边是翻页；两处各写一遍式子迟早会漂）。行数 < 1 时抬到 1。
static int btScanRows() {
    const int y = FONT_H + 8 + LINE_SPACING + (g_btState.connecting ? FONT_H : 0);
    int v = (STATUS_Y - y + FONT_H - 1) / FONT_H;  // 与 drawScan 同式
    return v < 1 ? 1 : v;
}
static int btPairedRows() {
    const int y = manageRowBase() + (g_btState.connecting ? FONT_H : 0);
    int v = (fabY() - 4 - y + FONT_H - 1) / FONT_H;  // 与 drawManageBase 同式
    return v < 1 ? 1 : v;
}
static int btHelpRows() {
    const int contentY = ui_title_baseline() + g_font.descent() + 12;
    int v = (STATUS_Y - contentY) / LINE_SPACING;  // 与 drawHelp 同式
    return v < 1 ? 1 : v;
}
static ListView btScanListView() {
    ListView lv;
    lv.sel = g_btState.selection;
    lv.first = g_btState.scroll;
    lv.count = g_bt.deviceCount();
    lv.rows = btScanRows();
    lv.top = FONT_H + 8 + LINE_SPACING + (g_btState.connecting ? FONT_H : 0) - g_font.ascent();
    lv.itemH = FONT_H;
    return lv;
}
static ListView btPairedListView() {
    ListView lv;
    lv.sel = g_btState.selection;
    lv.first = g_btState.scroll;
    lv.count = g_bt.pairedDeviceCount();
    lv.rows = btPairedRows();
    lv.top = manageRowBase() + (g_btState.connecting ? FONT_H : 0) - g_font.ascent();
    lv.itemH = FONT_H;
    return lv;
}
static bool fabHit(int x, int y) {
    if (!app_bt_enabled()) return false;   // 蓝牙关着时这个按钮不存在，别让它接点击
    return x >= fabX() && x < fabX() + fabW() && y >= fabY() && y < fabY() + fabH();
}

// 长按菜单的浮层框：宽度取最长一项，标题行（设备名）+ 分隔线 + 菜单项。
static void btMenuBoxRect(int *bx, int *by, int *bw, int *bh) {
    int w = 0;
    for (int i = 0; i < BT_MENU_N; i++) {
        int tw = g_font.textWidth(BT_MENU_LABELS[i]) + 2 * FONT_H;
        if (tw > w) w = tw;
    }
    const BtPairedDevice *p = g_bt.getPairedDevice(g_btState.menuTarget);
    if (p) {
        int tw = g_font.textWidth(p->name) + 2 * FONT_H;
        if (tw > w) w = tw;
    }
    if (w < 200) w = 200;
    if (w > SCREEN_W - 32) w = SCREEN_W - 32;
    *bw = w;
    *bh = 8 + FONT_H + 6 + BT_MENU_N * LINE_SPACING + 8;
    *bx = (SCREEN_W - w) / 2;
    *by = (SCREEN_H - *bh) / 2;
}
// 菜单项 i 的文字基线（绘制与命中共用）。
static int btMenuRowY(int by, int i) {
    int sepY = by + 8 + FONT_H + 2;   // 标题行下沿的分隔线
    return sepY + 6 + g_font.ascent() + i * LINE_SPACING;
}
// 点按落在菜单第几项上；点在浮层外返回 -1。
static int btMenuRowAt(int x, int y) {
    int bx, by, bw, bh;
    btMenuBoxRect(&bx, &by, &bw, &bh);
    if (x < bx || x >= bx + bw || y < by || y >= by + bh) return -1;
    for (int i = 0; i < BT_MENU_N; i++) {
        int ry = btMenuRowY(by, i);
        if (y >= ry - g_font.ascent() - 2 && y < ry - g_font.ascent() + FONT_H + 2) return i;
    }
    return -1;
}

// ── 动作（键盘与长按菜单共用）────────────────────────────────────────────

// 全局蓝牙开关：拆栈/起栈在 app_bt_set_enabled 里（main.cpp，可能阻塞到几秒，
// 见那边的注释）。这里只负责把本界面的临时态收干净 —— 扫描页依赖协议栈，关的时候
// 必须退回管理页；连接/扫描的进行中标记也一并清掉，否则再打开时界面会停在
// "正在连接…" 那种假状态上。
static void btToggleEnabled() {
    const bool on = !app_bt_enabled();
    // 开回来时先把名单读好，**再**起栈：起栈后的 bt_init 任务也会读这份名单，而
    // loadPairedDevices 直接写 s_paired/s_paired_count（没上锁），两个线程同时写会
    // 写出半新半旧的列表。读在起栈之前，就永远撞不上那个任务。
    if (on) g_bt.loadPairedDevices();
    app_bt_set_enabled(on);
    if (g_btState.mode == BT_SCAN) {
        g_btState.mode = BT_MANAGE;
        g_btState.scanning = false;
    }
    g_btState.connecting = false;
    g_btState.conn_start_ms = 0;
    g_btState.statusMsg[0] = '\0';
    g_btState.selection = 0;
    g_btState.scroll = 0;
    g_btState.menuOpen = false;
    g_btState.menuTarget = -1;
}

static void btStartScan() {
    g_btState.mode = BT_SCAN;
    g_btState.connecting = false;
    g_btState.conn_start_ms = 0;
    g_btState.statusMsg[0] = '\0';
    g_btState.selection = 0;
    g_btState.scroll = 0;
    g_btState.scanning = true;
    g_bt.scanDevices();
}

static void btConnectPairedAt(int idx) {
    const BtPairedDevice *p = g_bt.getPairedDevice(idx);
    if (!p) return;
    g_btState.connecting = true;
    g_btState.conn_start_ms = esp_timer_get_time() / 1000;
    snprintf(g_btState.statusMsg, sizeof(g_btState.statusMsg), "正在连接 %s...", p->name);
    g_bt.connectBDA(p->bda, p->addr_type);
}

static void btDeletePairedAt(int idx) {
    const BtPairedDevice *p = g_bt.getPairedDevice(idx);
    if (!p) return;
    if (g_bt.connectedPairedIndex() == idx) g_bt.disconnect();
    g_bt.removePairedDevice(p->bda);
    if (g_btState.selection >= g_bt.pairedDeviceCount())
        g_btState.selection = g_bt.pairedDeviceCount() - 1;
    if (g_btState.selection < 0) g_btState.selection = 0;
    g_btState.statusMsg[0] = '\0';
    g_btState.menuTarget = -1;
}

// ── 绘制 ─────────────────────────────────────────────────────────────────
// 整屏：标题 + 提示行 + 列表 + 浮动按钮 + 状态栏。**不含推屏**——长按菜单要在
// 这张底图上再叠浮层，一次提交只能有一个绘制目标，所以 commit 交给调用方。
// 全局蓝牙开关那一行：左"蓝牙"，右侧一枚开/关药丸。与浮动按钮同一套样子 ——
// 黑底白字 = 开（活的），白底黑框黑字 = 关（死的），一眼能看出状态。
static void drawBtSwitchRow() {
    const bool on = app_bt_enabled();
    ui_draw_text(8, btSwitchRowY(), "蓝牙", false);
    const char *t = on ? "开" : "关";
    int x, y, w, h;
    btSwitchPill(&x, &y, &w, &h);
    const int tw = g_font.textWidth(t);
    const int ty = y + (h - FONT_H) / 2 + g_font.ascent();
    if (on) {
        u8g2_SetDrawColor(g_u8g2, 0);
        u8g2_DrawBox(g_u8g2, x, y, w, h);
        u8g2_SetDrawColor(g_u8g2, 1);
        u8g2_DrawFrame(g_u8g2, x + 2, y + 2, w - 4, h - 4);   // 内描边：按钮而不是色块
        g_font.drawText(x + (w - tw) / 2, ty, t, true);
        u8g2_SetDrawColor(g_u8g2, 0);
    } else {
        u8g2_SetDrawColor(g_u8g2, 0);
        u8g2_DrawFrame(g_u8g2, x, y, w, h);
        g_font.drawText(x + (w - tw) / 2, ty, t, false);
    }
}

static void drawManageBase(const char *statusLeft) {
    ui_clear();
    ui_draw_text_centered(g_font.ascent(), "蓝牙设备管理", false, true);
    u8g2_DrawHLine(g_u8g2, 0, FONT_H + 4, SCREEN_W);
    const bool btOn = app_bt_enabled();
    drawBtSwitchRow();
    int y = manageRowBase();

    // 关着的时候列表必然是空的（协议栈拆了，什么都不连）：别画成"暂无已配对设备"，
    // 那会让人以为设备丢了。直说关着，并指路怎么开。
    if (!btOn) {
        y += FONT_H / 2;
        ui_draw_text_centered(y, "蓝牙已关闭", true); y += FONT_H + 4;
        ui_draw_text_centered(y, "键盘/遥控器不可用"); y += FONT_H;
        ui_draw_text_centered(y, "点上面那一行或按 B 打开");
        ui_draw_status(statusLeft, "?:帮助");
        return;
    }

    if (g_btState.connecting) {
        ui_draw_text_centered(y, g_btState.statusMsg, true); y += FONT_H;
    }

    int n = g_bt.pairedDeviceCount();
    int connIdx = g_bt.connectedPairedIndex();
    if (n == 0) {
        ui_draw_text_centered(y, "暂无已配对设备"); y += FONT_H;
        ui_draw_text_centered(y, "点右下角添加"); y += FONT_H;
    } else {
        ListView lv = btPairedListView();
        listViewFollow(lv);
        g_btState.selection = lv.sel;
        g_btState.scroll = lv.first;
        const int visible = lv.rows;

        for (int i = 0; i < visible && (g_btState.scroll + i) < n; i++) {
            int idx = g_btState.scroll + i;
            const BtPairedDevice *p = g_bt.getPairedDevice(idx);
            char buf[48];
            if (idx == connIdx)
                snprintf(buf, sizeof(buf), "\xe2\x97\x8f %s", p->name);  // ●
            else
                snprintf(buf, sizeof(buf), "  %s", p->name);
            bool sel = (idx == g_btState.selection);
            ui_draw_text(8, y + i * FONT_H, buf, sel);
        }
    }

    // 浮动按钮：黑底白字（与菜单选中项同一套配色）。
    {
        const int x = fabX(), y2 = fabY(), w = fabW(), h = fabH();
        const char *t = "+ 添加设备";
        u8g2_SetDrawColor(g_u8g2, 0);
        u8g2_DrawBox(g_u8g2, x, y2, w, h);
        u8g2_SetDrawColor(g_u8g2, 1);
        u8g2_DrawFrame(g_u8g2, x + 2, y2 + 2, w - 4, h - 4);   // 内描边：看着像按钮而不是色块
        int tw = g_font.textWidth(t);
        g_font.drawText(x + (w - tw) / 2, y2 + (h - FONT_H) / 2 + g_font.ascent(), t, true);
        u8g2_SetDrawColor(g_u8g2, 0);
    }

    ui_draw_status(statusLeft, "?:帮助");
}

static void drawManage() {
    const char *sl;
    if (!app_bt_enabled())
        sl = "B:打开蓝牙 q/Esc:返回";
    else
        sl = g_btState.menuOpen ? "Enter:确定 Esc:关闭" : "长按设备:菜单 Enter:连接";
    drawManageBase(sl);
    ui_commit();
}

// 长按设备行弹出的操作菜单：底图照画，浮层盖在上面。
static void drawBtMenu() {
    drawManageBase("Enter:确定 Esc:关闭");

    int bx, by, bw, bh;
    btMenuBoxRect(&bx, &by, &bw, &bh);
    u8g2_SetDrawColor(g_u8g2, 1);
    u8g2_DrawBox(g_u8g2, bx, by, bw, bh);
    u8g2_SetDrawColor(g_u8g2, 0);
    u8g2_DrawFrame(g_u8g2, bx, by, bw, bh);

    // 标题写设备名：不写清楚是哪台，删除就是盲删。
    const BtPairedDevice *p = g_bt.getPairedDevice(g_btState.menuTarget);
    char title[48];
    snprintf(title, sizeof(title), "%s", p ? p->name : "设备");
    ui_draw_text(bx + 10, by + 8 + g_font.ascent(), title, false, true);
    u8g2_DrawHLine(g_u8g2, bx + 4, by + 8 + FONT_H + 2, bw - 8);

    for (int i = 0; i < BT_MENU_N; i++) {
        const char *lb = BT_MENU_LABELS[i];
        int y = btMenuRowY(by, i);
        int tx = bx + (bw - g_font.textWidth(lb)) / 2;
        if (i == g_btState.menuSel) {
            u8g2_SetDrawColor(g_u8g2, 0);
            u8g2_DrawBox(g_u8g2, bx + 4, y - g_font.ascent() - 2, bw - 8, FONT_H + 4);
            u8g2_SetDrawColor(g_u8g2, 1);
            g_font.drawText(tx, y, lb, true);
            u8g2_SetDrawColor(g_u8g2, 0);
        } else {
            g_font.drawText(tx, y, lb, false);
        }
    }
    ui_commit();
}

// ── Screen entry points ──────────────────────────────────────────────────
void screen_bt_manage_init() {
    g_btState.mode = BT_MANAGE;
    g_btState.selection = 0;
    g_btState.scroll = 0;
    g_btState.scanning = false;
    g_btState.connecting = false;
    g_btState.conn_start_ms = 0;
    g_btState.statusMsg[0] = '\0';
    g_btState.showHelp = false;
    g_btState.helpScroll = 0;
    g_btState.menuOpen = false;
    g_btState.menuSel = 0;
    g_btState.menuTarget = -1;
    g_bt.loadPairedDevices();
}

static void drawScan() {
    ui_clear();
    ui_draw_text_centered(g_font.ascent(), "扫描添加设备", false, true);
    u8g2_DrawHLine(g_u8g2, 0, FONT_H + 4, SCREEN_W);
    int y = FONT_H + 8 + LINE_SPACING;

    if (g_btState.scanning) {
        ui_draw_text_centered(y, "正在扫描蓝牙键盘..."); y += FONT_H;
        if (y + FONT_H <= SCREEN_H)
            ui_draw_text_centered(y, "请确保键盘处于配对模式");
    } else {
        if (g_btState.connecting) {
            ui_draw_text_centered(y, g_btState.statusMsg, true); y += FONT_H;
        }
        int n = g_bt.deviceCount();
        if (n == 0) {
            ui_draw_text_centered(y, "未找到蓝牙键盘"); y += FONT_H;
            ui_draw_text_centered(y, "Esc 返回后重试");
        } else {
            ListView lv = btScanListView();
            listViewFollow(lv);
            g_btState.selection = lv.sel;
            g_btState.scroll = lv.first;
            const int visible = lv.rows;

            for (int i = 0; i < visible && (g_btState.scroll + i) < n; i++) {
                int idx = g_btState.scroll + i;
                auto *dev = g_bt.getDevice(idx);
                bool sel = (idx == g_btState.selection);
                char buf[48];
                snprintf(buf, sizeof(buf), "  %s", dev->name);
                ui_draw_text(8, y + i * FONT_H, buf, sel);
            }
        }
    }

    ui_draw_status("Enter:连接 Esc:返回", "?:帮助");
    ui_commit();
}

// ── 快捷键帮助对话框 ─────────────────────────────────────────────────────
static const char *BT_HELP_LINES[] = {
    "── 已配对设备管理 ──",
    "蓝牙    最上面一行,开/关总开关",
    "B      同上(关 = 不再连任何设备)",
    "长按设备  弹出菜单(连接/删除)",
    "+添加设备 右下角浮动按钮",
    "a      扫描添加设备",
    "d      删除选中配对",
    "Enter  连接选中设备",
    "?      帮助",
    "q/Esc  返回",
    "",
    "── 扫描添加设备 ──",
    "Enter  连接选中设备",
    "q/Esc  返回",
    "",
    "── 物理按键 ──",
    "USER 短按 上移",
    "BOOT 短按 下移",
    "USER 双击 添加设备",
    "BOOT 双击 删除配对",
    "USER 长按 连接",
    "BOOT 长按 Esc退出",
};
static const int BT_HELP_LINE_COUNT = sizeof(BT_HELP_LINES) / sizeof(BT_HELP_LINES[0]);

static void drawHelp() {
    ui_clear();
    ui_draw_text_centered(ui_title_baseline(), "快捷键帮助", false, true);
    u8g2_DrawHLine(g_u8g2, 0, ui_title_baseline() + g_font.descent() + 4, SCREEN_W);
    int contentY = ui_title_baseline() + g_font.descent() + 12;
    int contentMaxY = STATUS_Y;
    int maxVis = (contentMaxY - contentY) / LINE_SPACING;
    if (maxVis < 1) maxVis = 1;
    int maxScroll = BT_HELP_LINE_COUNT - maxVis;
    if (maxScroll < 0) maxScroll = 0;
    if (g_btState.helpScroll > maxScroll) g_btState.helpScroll = maxScroll;
    if (g_btState.helpScroll < 0) g_btState.helpScroll = 0;
    for (int i = 0; i < maxVis && (g_btState.helpScroll + i) < BT_HELP_LINE_COUNT; i++) {
        int ly = contentY + i * LINE_SPACING;
        const char *line = BT_HELP_LINES[g_btState.helpScroll + i];
        bool isHeader = ((unsigned char)line[0] == 0xE2);
        ui_draw_text(12, ly + g_font.ascent(), line, false, isHeader);
    }
    ui_draw_status("Esc返回", "");
    ui_commit();
}

AppState screen_bt_manage_handle(int key, ScreenContext &ctx) {
    // 触摸上下滑的翻页键（主循环不再替本界面回退成单步）：扫描列表 / 已配对列表 /
    // 帮助正文都按屏翻（下面各自接 KEY_PAGE_*）；只有长按弹的设备菜单保持单步。
    if (key == KEY_PAGE_UP || key == KEY_PAGE_DOWN) {
        if (g_btState.menuOpen) key = (key == KEY_PAGE_UP) ? KEY_UP : KEY_DOWN;
    }

    // 扫描完成跟踪
    if (g_btState.scanning && !g_bt.isScanning()) {
        g_btState.scanning = false;
        g_btState.selection = 0;
        g_btState.scroll = 0;
    }

    // 连接成功或超时清除"连接中"状态
    const int64_t CONN_TIMEOUT_MS = 8000;
    if (g_btState.connecting && g_btState.conn_start_ms > 0) {
        if (g_bt.isConnected()) {
            g_btState.connecting = false;
            g_btState.conn_start_ms = 0;
            g_btState.statusMsg[0] = '\0';
        } else {
            int64_t elapsed = (esp_timer_get_time() / 1000) - g_btState.conn_start_ms;
            if (elapsed > CONN_TIMEOUT_MS) {
                g_btState.connecting = false;
                g_btState.conn_start_ms = 0;
                g_btState.statusMsg[0] = '\0';
            }
        }
    }

    // ── 快捷键帮助对话框 ──
    if (g_btState.showHelp) {
        if (key == KEY_UP) {
            if (g_btState.helpScroll > 0) g_btState.helpScroll--;
        } else if (key == KEY_DOWN) {
            g_btState.helpScroll++;
        } else if (key == KEY_PAGE_UP) {
            g_btState.helpScroll -= btHelpRows();
            if (g_btState.helpScroll < 0) g_btState.helpScroll = 0;
        } else if (key == KEY_PAGE_DOWN) {
            g_btState.helpScroll += btHelpRows();  // 上限在画的时候夹住
        } else if (key == 0x1B || key == 'q' || key == 'Q' || key == '?') {
            g_btState.showHelp = false;
        }
        drawHelp();
        return APP_BT_MANAGE;
    }

    // ── 全局蓝牙开关 ──
    // 放在扫描分支**前面**：在扫描页里按 B 也应该是"把蓝牙关掉"（顺手退回管理页），
    // 而不是留在那个连协议栈都没了的页面上。触摸点法见下面那一段注释。
    if (key == 'b' || key == 'B') {
        btToggleEnabled();
        drawManage();
        return APP_BT_MANAGE;
    }
    // 点开关那一行 = 切换。**必须拦在下面"点哪行连哪行"之前**——那一段对落在列表
    // 上方的点是"没点中行"，然后照样拿旧 selection 去连，于是点开关会顺手连上设备。
    // 先 peek：input_tap_xy 是取走即清，没点在这一行的话落点要原样留给下面那一段，
    // 否则"点哪行连哪行"会因为落点被人先取走而退化成"连当前选中项"。
    if (key == 0x0A || key == 0x0D) {
        int tx = 0, ty = 0;
        if (input_tap_peek_xy(&tx, &ty) && btSwitchRowHit(ty)) {
            input_tap_xy(&tx, &ty);   // 命中开关行：这颗落点归我，吃掉
            btToggleEnabled();
            drawManage();
            return APP_BT_MANAGE;
        }
    }

    // 关着的时候，设备那一套动作（连接/删除/扫描/列表导航）全不接：协议栈已经拆了，
    // 列表也不画。只剩开关、返回和帮助。
    if (!app_bt_enabled()) {
        if (key == '?') {
            g_btState.helpScroll = 0;
            g_btState.showHelp = true;
            drawHelp();
            return APP_BT_MANAGE;
        }
        if (key == 'q' || key == 'Q' || key == 0x1B) {
            ctx.nextState = g_quickEdit ? APP_SETTINGS : APP_MAIN;
            return ctx.nextState;
        }
        // 关着时点哪儿都不动作，但落点照吞（input_tap_xy 取走即清）：留着不取，
        // 下一帧换别的键进来时它还在，会变成一颗不知哪来的"幽灵点按"。
        if (key == 0x0A || key == 0x0D) { int tx, ty; input_tap_xy(&tx, &ty); }
        drawManage();
        return APP_BT_MANAGE;
    }

    // ── 扫描模式(添加设备) ──
    if (g_btState.mode == BT_SCAN) {
        if (key == 'q' || key == 'Q' || key == 0x1B) {
            g_btState.mode = BT_MANAGE;
            g_btState.connecting = false;
            g_btState.statusMsg[0] = '\0';
            g_btState.selection = 0; g_btState.scroll = 0;
            g_bt.loadPairedDevices();
        } else if (key == '?') {
            g_btState.helpScroll = 0;
            g_btState.showHelp = true;
            drawHelp();
            return APP_BT_MANAGE;
        } else if (g_bt.isConnected()) {
            // 连接成功 → 返回管理列表并选中该设备
            g_btState.mode = BT_MANAGE;
            g_btState.connecting = false;
            g_btState.statusMsg[0] = '\0';
            g_bt.loadPairedDevices();
            int ci = g_bt.connectedPairedIndex();
            g_btState.selection = (ci >= 0) ? ci : 0;
            g_btState.scroll = 0;
        } else if (!g_btState.scanning && !g_btState.connecting) {
            if (key == KEY_UP || key == KEY_DOWN || key == KEY_PAGE_UP || key == KEY_PAGE_DOWN ||
                key == KEY_HOME || key == KEY_END) {
                ListView lv = btScanListView();
                if (listViewKey(lv, key)) {
                    listViewFollow(lv);
                    g_btState.selection = lv.sel;
                    g_btState.scroll = lv.first;
                }
            } else if (key == 0x0A || key == 0x0D) {
                int n = g_bt.deviceCount();
                if (n > 0 && g_btState.selection < n) {
                    g_btState.connecting = true;
                    g_btState.conn_start_ms = esp_timer_get_time() / 1000;
                    snprintf(g_btState.statusMsg, sizeof(g_btState.statusMsg),
                             "正在连接 %s...", g_bt.getDevice(g_btState.selection)->name);
                    g_bt.connectDevice(g_btState.selection);
                }
            }
        }
        drawScan();
        return APP_BT_MANAGE;
    }

    // ── 已配对设备管理模式(默认) ──

    // 长按菜单浮层：↑↓/点按移动，回车执行，Esc 关掉。
    if (g_btState.menuOpen) {
        if (key == KEY_UP) {
            if (g_btState.menuSel > 0) g_btState.menuSel--;
        } else if (key == KEY_DOWN) {
            if (g_btState.menuSel < BT_MENU_N - 1) g_btState.menuSel++;
        } else if (key == 0x0A || key == 0x0D) {
            int tx, ty;
            if (input_tap_xy(&tx, &ty)) {
                int r = btMenuRowAt(tx, ty);
                if (r < 0) {   // 点浮层外 = 关掉菜单
                    g_btState.menuOpen = false;
                    drawManage();
                    return APP_BT_MANAGE;
                }
                g_btState.menuSel = r;
            }
            int act = g_btState.menuSel;
            int target = g_btState.menuTarget;
            g_btState.menuOpen = false;
            if (act == BT_MENU_CONNECT) {
                btConnectPairedAt(target);
            } else if (act == BT_MENU_DELETE) {
                btDeletePairedAt(target);
                // 删完把选中行挪到被删的那台原位置附近，避免"看着没变但选中的是别人"
                if (g_bt.pairedDeviceCount() > 0 && target < g_bt.pairedDeviceCount())
                    g_btState.selection = target;
            }
        } else if (key == 0x1B || key == 'q' || key == 'Q') {
            g_btState.menuOpen = false;
        }
        // ↑↓ 只挪 menuSel，浮层还开着 —— 必须重画浮层（drawBtMenu），否则选择框
        // 纹丝不动；回车/Esc 把 menuOpen 落了地，这时才回到平铺管理页。
        if (g_btState.menuOpen) drawBtMenu(); else drawManage();
        return APP_BT_MANAGE;
    }

    // 长按设备行 → 弹出操作菜单。按下点落在哪一行就操作哪一行，与键盘"先选中再
    // 按 d/Enter"完全等价（先把 selection 指过去，菜单里的动作才能复用那两个入口）。
    if (key == KEY_TOUCH_LONG) {
        int lx = 0, ly = 0;
        if (input_tap_xy(&lx, &ly) && !fabHit(lx, ly)) {
            int n = g_bt.pairedDeviceCount();
            if (n > 0) {
                ListView lv = btPairedListView();
                listViewFollow(lv);
                const int hit = (ly < fabY() - 4) ? listViewHitAt(lv, ly) : -1;
                if (hit >= 0 && hit < n) {
                    g_btState.selection = hit;
                    g_btState.menuTarget = g_btState.selection;
                    g_btState.menuSel = 0;
                    g_btState.menuOpen = true;
                    drawBtMenu();   // 弹的是浮层，不是平铺页（以前这里调 drawManage，菜单永远看不见）
                    return APP_BT_MANAGE;
                }
            }
        }
    }

    if (key == 'q' || key == 'Q' || key == 0x1B) {
        if (g_btState.connecting) g_bt.disconnect();
        ctx.nextState = g_quickEdit ? APP_SETTINGS : APP_MAIN;
        return ctx.nextState;
    } else if (key == 'a' || key == 'A') {
        btStartScan();
    } else if (key == 'd' || key == 'D') {
        btDeletePairedAt(g_btState.selection);
    } else if (key == '?') {
        g_btState.helpScroll = 0;
        g_btState.showHelp = true;
        drawHelp();
        return APP_BT_MANAGE;
    } else if (g_btState.connecting) {
        // 连接中忽略导航键
    } else if (key == KEY_UP || key == KEY_DOWN || key == KEY_PAGE_UP || key == KEY_PAGE_DOWN ||
               key == KEY_HOME || key == KEY_END) {
        ListView lv = btPairedListView();
        if (listViewKey(lv, key)) {
            listViewFollow(lv);
            g_btState.selection = lv.sel;
            g_btState.scroll = lv.first;
        }
    } else if (key == 0x0A || key == 0x0D) {
        int n = g_bt.pairedDeviceCount();
        int tx = 0, ty = 0;
        const bool tapped = input_tap_xy(&tx, &ty);
        if (tapped && fabHit(tx, ty)) {
            btStartScan();          // 浮动按钮 = 扫描添加设备，与键盘 'a' 同一条路
        } else if (n > 0) {
            if (tapped) {
                // 点按哪一行就连哪一行（以前只认"先选中再回车"，点别的行会连错设备）
                const int top = manageRowBase() - g_font.ascent();
                int r = (ty - top) / FONT_H;
                if (ty >= top && r >= 0 && g_btState.scroll + r < n && ty < fabY() - 4)
                    g_btState.selection = g_btState.scroll + r;
            }
            btConnectPairedAt(g_btState.selection);
        }
    }

    drawManage();
    return APP_BT_MANAGE;
}
