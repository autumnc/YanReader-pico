#pragma once
// ── 列表的选择 / 滚动 / 命中（ListView）──────────────────────────────────
// 全仓的列表页都长一个样：一个选中下标 + 一段"上下/翻页怎么动、动完夹在哪儿"的
// 样板 + 一个"窗口从第几行开始"的公式 + 一个"点按 y 落在第几行"的换算。四件事各自
// 手写一遍，于是攒出两类反复出现的坑：
//
//   · **夹边界写漏**（`sel + 1` 少夹一次；count==0 时算出负下标）；
//   · **绘制与命中用了两个式子** —— 列表画在第 y 行，点上去却按另一个公式算行号，
//     症状是"点哪行开隔壁那行"，而且**只有滚动过之后才显形**（第一屏时两者恰好一致，
//     所以自测时看不出来）。全仓现在有 40 处 tap→行号，其中好几处就是"把渲染函数里
//     那几行窗口公式抄了一遍"（阅读模式文件浏览的 fbRowAtY 是照 renderFileBrowser 抄的，
//     两边的 maxRows/start 必须一直保持同步）。
//
// 所以这一层只做三件事，**不做列表控件**：不管数据、不管画、不管脏标记。宿主仍旧持有
// 自己那份字段（`st.fbSel` 之类），只把算术交出来。调用点长这样：
//
//   ListView lv = fbListView();     // 几何（count/top/itemH/rows），渲染与命中共用
//   lv.sel = st.fbSel;
//   if (listViewKey(lv, key)) { st.fbSel = lv.sel; st.dirty = 1; return; }
//
// **渲染和命中要传同一个 builder 造出来的 lv** —— 这才是这一层想买的东西。
//
// 已经迁到这一层的（阅读模式里各自写了一个几何 builder，渲染/按键/点按三处共用）：
// 文件浏览、OPDS、最近阅读、目录、书签、脚注、阅读菜单、设置标签、词典下载、
// WiFi 管理、资源下载、按键映射、自定义状态栏、WiFi 传书、关于页、微信读书书架、
// 微信读书书目菜单、统计（主页/单本/某天/统计设置）、书架搜索、笔记搜索。
//
// **故意没进来的三类**（几何/语义对不上，硬套就是假统一）：
//   · **整页像素滚动**（"更多详情"`rdStatsMoreContentH`）：滚的是像素不是行；
//   · **行高可变的列表**（笔记标签页：分组书名行 + 两行笔记行，`rdNoteRowH`）——
//     它是"逐行累加看装不装得下"，没有 itemH 可言；
//   · **网格**（书架封面墙：行/列两维 + 列数随宽度变）与**弹层选择器**
//     （`rdPickBox`/`rdPickRowTop`，坐标是"相对浮层内框"而不是屏幕）；
//   · 还有各屏的**表单光标**（上下键在字段间跳、左右键改值，不是列表）。
#include "pjournal_app.h"  // KEY_UP/DOWN/PAGE_*/HOME/END

struct ListView {
    int sel = 0;    // 选中项下标（0 基）
    int first = 0;  // 可见窗口第一行的下标（滚动偏移）
    int count = 0;  // 总行数（0 = 空列表）
    int rows = 1;   // 可见行数（窗口高 / 行高，至少 1）
    int top = 0;    // 首行**上沿**的 y（不是基线、不是行心 —— 与绘制同一个基准）
    int itemH = 1;  // 行高（含行距），必须 > 0
    int page = 8;   // 翻页步长
};

// 上下/翻页/首尾 → 改 sel，夹在 [0, count)。返回 true = 这个键归列表。
// **边界上按一下也算 true**（键被吃掉、只是没动）—— 各屏原来的写法就是
// `if (key == KEY_UP) { sel = max(0, sel-1); ...; return; }`，在顶上也照样 return。
// count <= 0 时返回 false：空列表没有"选中项"可动，键留给调用方。
inline bool listViewKey(ListView &lv, int key) {
    if (lv.count <= 0) return false;
    const int last = lv.count - 1;
    const int step = (lv.page > 0) ? lv.page : 1;
    switch (key) {
        case KEY_UP:        lv.sel -= 1; break;
        case KEY_DOWN:      lv.sel += 1; break;
        case KEY_PAGE_UP:   lv.sel -= step; break;
        case KEY_PAGE_DOWN: lv.sel += step; break;
        case KEY_HOME:      lv.sel = 0; break;
        case KEY_END:       lv.sel = last; break;
        default: return false;
    }
    if (lv.sel < 0) lv.sel = 0;
    if (lv.sel > last) lv.sel = last;
    return true;
}

// 把 sel 夹回 [0,count)、rows 抬到 ≥1（两个窗口函数共用的前半段）。
inline void listViewClamp(ListView &lv) {
    if (lv.rows < 1) lv.rows = 1;
    if (lv.count <= 0) { lv.sel = 0; lv.first = 0; return; }
    const int last = lv.count - 1;
    if (lv.sel < 0) lv.sel = 0;
    if (lv.sel > last) lv.sel = last;
}

// 跟随式窗口：选中项滚出可见区才滚（`first` 是**持久**字段，存在宿主自己的 st 里）。
// 计划模式的列表、蓝牙设备管理、查看过往日记都是这个模型。
inline void listViewFollow(ListView &lv) {
    listViewClamp(lv);
    if (lv.count <= 0) return;
    const int maxFirst = (lv.count > lv.rows) ? lv.count - lv.rows : 0;
    if (lv.first < 0) lv.first = 0;
    if (lv.first > maxFirst) lv.first = maxFirst;
    if (lv.sel < lv.first) lv.first = lv.sel;
    if (lv.sel >= lv.first + lv.rows) lv.first = lv.sel - lv.rows + 1;
}

// 居中式窗口：让选中项尽量待在窗口正中，首尾由 [0, count-rows] 夹住（`first` 是
// **算出来的**，不用存）。阅读模式的文件浏览 / OPDS / 词典下载都是这个模型 ——
// 它们原来在渲染函数和命中函数里各算了一遍同一个式子。
inline void listViewCenter(ListView &lv) {
    listViewClamp(lv);
    if (lv.count <= 0) return;
    const int maxFirst = (lv.count > lv.rows) ? lv.count - lv.rows : 0;
    lv.first = lv.sel - lv.rows / 2;
    if (lv.first < 0) lv.first = 0;
    if (lv.first > maxFirst) lv.first = maxFirst;
}

// 滚动式窗口（**没有选中项**，宿主只存"顶行下标"）。用 lv.first 装那个偏移量：
//
//   lv.first = st.aboutTop;   // 宿主那份偏移
//   listViewScroll(lv);       // 夹回 [0, count-rows] —— 渲染循环与点按命中都读 lv.first
//
// 与 listViewCenter 的区别：居中式是"围着我转"（first 由 sel 算出来），滚动式是"我自己
// 就是位置"（first 是宿主状态）。统计子页 / 关于页原来是"渲染夹一次、按键再夹一次"，
// 两处的边界写法稍微一漂，点按就会整体错一行。
inline void listViewScroll(ListView &lv) {
    listViewClamp(lv);
    if (lv.count <= 0) { lv.first = 0; return; }
    const int maxFirst = (lv.count > lv.rows) ? lv.count - lv.rows : 0;
    if (lv.first < 0) lv.first = 0;
    if (lv.first > maxFirst) lv.first = maxFirst;
}

// 点按 y → **下标**（-1 = 没点在列表的任何一行上：标题/空白/后面的浮动按钮那一条）。
// 用 lv.top/itemH/rows/first 算，所以调用方必须先按当前模型算好 first
// （listViewFollow/listViewCenter/listViewScroll），渲染循环也用同一个 lv —— 两边就不会错位。
// 只有**画出来了**的行才可点：窗口外的行（少半行的余量条）一律返回 -1。
inline int listViewHitAt(const ListView &lv, int y) {
    if (lv.count <= 0 || lv.itemH <= 0) return -1;
    if (y < lv.top) return -1;
    const int row = (y - lv.top) / lv.itemH;
    if (row >= lv.rows) return -1;
    const int idx = lv.first + row;
    return (idx < 0 || idx >= lv.count) ? -1 : idx;
}
