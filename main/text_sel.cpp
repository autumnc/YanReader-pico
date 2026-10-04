#include "text_sel.h"

#include "clipboard.h"
#include "edit_menu.h"
#include "pjournal_app.h"   // KEY_* 键码
#include "ui_helpers.h"

#include <algorithm>

extern u8g2_t *g_u8g2;

extern "C" {
extern void u8g2_SetDrawColor(void *u8g2, int color);
extern void u8g2_DrawBox(void *u8g2, int x, int y, int w, int h);
}

// ── 会话状态 ────────────────────────────────────────────────────────────
// 选区一律用**缓冲里的字节偏移**表示：宿主给行表、模块算字节，双方都不用互相
// 翻译"第几行第几列"，多行/折行的宿主也就不必各自维护第二套选区模型。
static bool s_active = false;
static int  s_selStart = 0;     // 闭开区间 [start, end)
static int  s_selEnd = 0;

void textSelReset() {
    s_active = false;
    s_selStart = 0;
    s_selEnd = 0;
    editMenuClose();
    editMenuClosePicker();
}

bool textSelActive() { return s_active; }

// ── 与行表打交道 ────────────────────────────────────────────────────────
// 手指落在哪一行：取纵向最近的，容差 = 半字高 + 行距留白的一半 + 一点余量。
// 单行字段就退化成"贴着这一行"；多行字段行距里的空隙也算在行内（不然点不中）。
static int tsLineAtY(const TextSelView &view, int y) {
    int best = -1, bestD = 0;
    for (int i = 0; i < view.count; ++i) {
        const int cy = view.lines[i].baseline - g_font.ascent() + FONT_H / 2;
        const int d = (y >= cy) ? (y - cy) : (cy - y);
        if (best < 0 || d < bestD) { best = i; bestD = d; }
    }
    if (best < 0) return -1;
    const int tol = FONT_H / 2 + std::max(0, (LINE_SPACING - FONT_H) / 2) + 6;
    return (bestD <= tol) ? best : -1;
}

// 把选区/光标夹回缓冲范围内（宿主换过缓冲、或缓冲区被别处改短）。
static void tsClamp(const std::string &buf, int &cur) {
    const int n = (int)buf.size();
    if (s_selStart < 0) s_selStart = 0;
    if (s_selEnd > n) s_selEnd = n;
    if (s_selStart > s_selEnd) s_selStart = s_selEnd;
    if (cur < 0) cur = 0;
    if (cur > n) cur = n;
}

static std::string tsSelected(const std::string &buf) {
    if (s_selEnd <= s_selStart) return std::string();
    return buf.substr(s_selStart, s_selEnd - s_selStart);
}

// 单行字段里不能有换行：粘贴进来的多行内容把换行/制表符折成空格。
static std::string tsFlatten(const std::string &in) {
    std::string o;
    o.reserve(in.size());
    for (char c : in) o += (c == '\n' || c == '\r' || c == '\t') ? ' ' : c;
    return o;
}

// 有选区就整体替换，否则插在光标处；光标停在插入内容之后。
static void tsReplaceSelection(std::string &buf, int &cur, const std::string &text) {
    if (s_selEnd > s_selStart) {
        buf.erase(s_selStart, s_selEnd - s_selStart);
        cur = s_selStart;
        s_selEnd = s_selStart;
    }
    buf.insert(cur, text);
    cur += (int)text.size();
    s_selStart = s_selEnd = cur;
}

static void tsClose() {
    s_active = false;
    editMenuClose();
    editMenuClosePicker();
}

// ── 长按入场 ────────────────────────────────────────────────────────────
bool textSelBegin(std::string &buf, int &cur, const TextSelView &view, int x, int y) {
    (void)x;   // 选整行，横向位置不参与判定（多行宿主各行 x0 不同，纵向足够）

    // 会话已经开着时，再长按 = 收掉（同编辑器：长按收会话），而不是重新选一遍。
    if (s_active) {
        tsClose();
        return true;
    }
    const int li = tsLineAtY(view, y);
    if (li < 0) return false;   // 没落在任何一行上 → 交给宿主的老语义（长按=返回）

    tsClose();
    s_active = true;

    const TextSelLine &ln = view.lines[li];
    const int n = (int)buf.size();
    int a = ln.start, b = ln.end;
    if (a < 0) a = 0;
    if (a > n) a = n;
    if (b < a) b = a;
    if (b > n) b = n;
    // 行尾的换行也算进来：剪切一整行不会在缓冲里留下一个空行。
    if (b < n && buf[b] == '\n') ++b;

    s_selStart = a;
    s_selEnd = b;
    cur = b;                    // 光标停在行尾，反白就是这一整行
    if (s_selEnd <= s_selStart) cur = a;   // 空行：没得选，但按钮条照样弹（要粘贴）
    editMenuOpen(EM_FIELD);
    return true;
}

// ── 按钮条动作 ──────────────────────────────────────────────────────────
// EM_FIELD = {复制, 剪切, 粘贴, 全选}。动作与写作模式的 editorAction* 同一套语义：
// 都落在共享 clipboard 上，所以三模式之间互相看得见。
static void tsActivate(int btn, std::string &buf, int &cur, int bottom) {
    switch (btn) {
    case 0: {   // 复制
        const std::string sel = tsSelected(buf);
        if (sel.empty()) break;
        clipboardPush(sel);
        ui_toast_show(clipboardLastTruncated() ? "已复制（超长已截断）" : "已复制", bottom);
        break;
    }
    case 1: {   // 剪切
        const std::string sel = tsSelected(buf);
        if (sel.empty()) break;
        clipboardPush(sel);
        buf.erase(s_selStart, s_selEnd - s_selStart);
        cur = s_selStart;
        s_selEnd = s_selStart;
        ui_toast_show(clipboardLastTruncated() ? "已剪切（超长已截断）" : "已剪切", bottom);
        break;
    }
    case 2:   // 粘贴
        if (clipboardEmpty()) {
            ui_toast_show("粘贴板是空的", bottom);
            tsClose();
            return;
        }
        if (clipboardCount() > 1) {
            editMenuOpenPicker();   // 多条 → 挑一条，会话留着
            return;
        }
        tsReplaceSelection(buf, cur, tsFlatten(clipboardLatest()));
        ui_toast_show("已粘贴", bottom);
        break;
    case 3:   // 全选：只换选区，**不收会话**——接着就能复制/剪切/粘贴
        s_selStart = 0;
        s_selEnd = (int)buf.size();
        cur = s_selEnd;
        return;
    default:
        break;
    }
    tsClose();
}

// ── 按键 ────────────────────────────────────────────────────────────────
bool textSelHandleKey(std::string &buf, int &cur, const TextSelView &view, int key,
                      int tapX, int tapY, bool hasTap, std::string *status) {
    // 结果提示改走 ui_toast（就地白框），不再回 ctx.statusMessage——那条是全屏消息，
    // 复制一句话就整屏闪一下太吵。status 形参留着不动各宿主的调用点。
    (void)status;
    if (!s_active) return false;
    tsClamp(buf, cur);

    // ① 粘贴板列表：模态，全给它。
    if (editMenuPickerActive()) {
        if (key == KEY_UP || key == KEY_PAGE_UP) { editMenuPickerMove(-1); return true; }
        if (key == KEY_DOWN || key == KEY_PAGE_DOWN) { editMenuPickerMove(+1); return true; }
        if (key == KEY_TOUCH_DRAG) return true;
        if (key == 0x1B) { editMenuClosePicker(); return true; }
        if (key == '\n') {
            int idx = -1;
            if (hasTap) idx = editMenuPickerHit(tapX, tapY);
            else idx = editMenuPickerSel();
            editMenuClosePicker();
            if (idx >= 0) {
                tsReplaceSelection(buf, cur, tsFlatten(clipboardAt(idx)));
                ui_toast_show("已粘贴", view.bottom);
            }
            tsClose();
            return true;
        }
        return true;   // 列表开着时别的键一律吃掉
    }

    // ② 拖动帧：字段里没有两柄可拖，别让它漏到宿主去（会被当成选区/翻页）。
    if (key == KEY_TOUCH_DRAG) return true;

    // ③ 收会话：长按 / Esc / 确认键。
    if (key == KEY_TOUCH_LONG || key == KEY_LONG_CONFIRM || key == 0x1B) {
        tsClose();
        return true;
    }

    // ④ 上下键移按钮条高亮。
    if (key == KEY_UP || key == KEY_DOWN) {
        editMenuMove(key == KEY_UP ? -1 : +1);
        return true;
    }

    // ⑤ 退格：有选区就删掉整段（比一个个退快得多），否则放行给宿主。
    if ((key == 0x7F || key == 0x08) && s_selEnd > s_selStart) {
        buf.erase(s_selStart, s_selEnd - s_selStart);
        cur = s_selStart;
        s_selEnd = s_selStart;
        tsClose();
        return true;
    }

    // ⑥ 回车：点按 → 命中按钮条；无坐标回车 → 执行高亮项。
    if (key == '\n') {
        int btn = -1;
        if (hasTap) {
            btn = editMenuHit(tapX, tapY);
            if (btn < 0) { tsClose(); return true; }   // 点别处 = 收掉
        } else {
            btn = editMenuSel();
        }
        tsActivate(btn, buf, cur, view.bottom);
        return true;
    }

    // ⑦ 可打印字符：有选区就替换掉（"全选后直接打字"是常规操作），
    //    然后放行——宿主/IME 自己会把这个字符插到 cur 上。
    if (key >= 0x20 && key < 0x7F) {
        if (s_selEnd > s_selStart) {
            buf.erase(s_selStart, s_selEnd - s_selStart);
            cur = s_selStart;
            s_selEnd = s_selStart;
        }
        tsClose();
        return false;
    }

    // ⑧ 其它任何键（实体键盘/蓝牙）：收掉按钮条，放行给常规分支。
    tsClose();
    return false;
}

// ── 绘制 ────────────────────────────────────────────────────────────────
void textSelDraw(const std::string &buf, const TextSelView &view) {
    if (!s_active) return;

    int a = s_selStart, b = s_selEnd;
    if (a > b) std::swap(a, b);
    const int n = (int)buf.size();
    if (a < 0) a = 0;
    if (b > n) b = n;

    // 逐行反白：把选区按行切开，每行的 [lo,hi) 画成一块（XOR 同编辑器横排选区）。
    u8g2_SetDrawColor(g_u8g2, 2);
    for (int i = 0; i < view.count; ++i) {
        const TextSelLine &ln = view.lines[i];
        const int lo = std::max(a, ln.start);
        const int hi = std::min(b, ln.end);
        if (hi <= lo) continue;
        const int x1 = ln.x0 + g_font.textWidth(buf.substr(ln.start, lo - ln.start).c_str());
        const int x2 = ln.x0 + g_font.textWidth(buf.substr(ln.start, hi - ln.start).c_str());
        if (x2 > x1) u8g2_DrawBox(g_u8g2, x1, ln.baseline - g_font.ascent(), x2 - x1, FONT_H);
    }
    u8g2_SetDrawColor(g_u8g2, 1);

    editMenuSetBottom(view.bottom > 0 ? view.bottom : SCREEN_H);
    if (editMenuActive()) editMenuDraw();
    if (editMenuPickerActive()) editMenuPickerDraw();
}
