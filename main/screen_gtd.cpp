#include "screen_gtd.h"

#include "font_renderer.h"
#include "icon_font.h"   // 标签栏图标（NF-Propo 子集）
#include "tab_icons.h"

#include "json_parser.h"

#include "journal_storage.h"

#include "ui_helpers.h"
#include "ui_render.h"
#include "ui/list_view.h"

#include "ime/IME.h"
#include "ui/ime_field.h"   // 输入框绑定：落串/退格/光标的 UTF-8 算术（与写作/阅读共用一份）

#include "hw/input.h"   // input_tap_xy / input_drag_xy：触摸点按与拖动增量

#include "editor_vk.h"  // 编辑态虚拟键盘（与写作模式同一套，未连蓝牙时显示）

#include "text_sel.h"   // 单行输入框的触摸选字 / 粘贴板（三模式共享底层件）

#include "settings_manager.h"   // g_settings：计划模式独立方向（"gtd_orientation"）
#include "hw/auto_orient.h"  // auto_orient_initial：本模式的「自适应」方向
#include "hw/board.h"    // board_force_*/board_restore_orientation（模式方向切换）

#include "esp_timer.h"

#include <cstdio>

#include <cstring>

#include <ctime>

#include <algorithm>

#include <set>

#include <map>

#include <sys/stat.h>

#include <dirent.h>
#include "u8g2_shim.h"



extern "C" {

    // 图标字体直接写 4bpp 帧缓冲（与主菜单 drawMainMenuIcon 同一条路），不走 u8g2 的
    // 画点接口。返回的就是 epdiy 的 front_fb。

}



// ── Constants ────────────────────────────────────────────────────────────

#define DATA_DIR  "/sdcard/gtd"

#define DATA_FILE DATA_DIR "/gtd.json"

#define ARCHIVE_DIR DATA_DIR "/archive"



// 标签栏改成图标（tab_icons.h），文字标签不再上屏。顺序与 View 枚举一一对应：
//   收集箱 下一步 等待 项目 已完成
static const uint32_t kGtdTabIcons[] = {TAB_ICON_INBOX, TAB_ICON_NEXT, TAB_ICON_WAITING,
                                        TAB_ICON_PROJECT, TAB_ICON_DONE};

enum View { V_INBOX, V_NEXT, V_WAITING, V_PROJECT, V_COMPLETED, V_CONCAT };

// V_CONCAT used as count, after V_COMPLETED

static const int V_COUNT = 5;



static const char *STATUS_LABELS[] = {"todo", "doing", "done", "waiting"};

static const char *STATUS_DISPLAY[] = {"待办", "进行中", "已完成", "等待中"};

static const char *PRIORITY_LABELS[] = {"A", "B", "C"};

static const char *PRIORITY_DISPLAY[] = {"A 高", "B 中", "C 低"};



enum Mode { M_BROWSE, M_ADD, M_DETAIL, M_EDIT_FIELD, M_FILTER, M_RENAME, M_CONFIRM, M_ADD_PROJECT, M_RENAME_PROJECT, M_PICKER, M_CALENDAR, M_HELP, M_EDIT_NOTE, M_CONTEXT_MGR, M_TAG_MGR, M_ADD_CONTEXT, M_ADD_TAG, M_RENAME_CONTEXT, M_RENAME_TAG, M_SUMMARY, M_ARCHIVE, M_ITEM_MENU };



// Picker option for popup panel

struct PickerOpt {

    std::string value;   // actual JSON value

    std::string display; // display text

};



// ── State ────────────────────────────────────────────────────────────────

static struct {

    int view = 0;

    int sel = 0;

    int scroll = 0;

    int scrollFrac = 0;   // 平滑滚动：窗口顶边相对整行的像素偏移(0..LINE_SPACING-1)

    int mode = M_BROWSE;

    JsonValue data;           // full task data from file

    std::vector<int> filtered; // indices into data["tasks"] array



    // detail / edit

    int detailTaskIdx = -1;   // index in data["tasks"]

    int detailField = 0;



    // text editing

    std::string editBuf;

    int editCur = 0;

    // 可编辑字段在屏幕上的几何：**绘制时记下**，触摸编辑（长按选字 / 反白 / 粘贴板）
    // 直接用它做命中与反白，不必在别处再算一遍（各模式的基线各不相同）。
    // fieldX == 0 表示本帧没有可编辑字段——触摸编辑整块跳过。
    int fieldX = 0;

    int fieldBaseline = 0;

    bool imeActive = false;

    std::string pendingParent;

    std::string pendingProject;  // auto-set project for new task in project view



    // filter

    std::string filterText;



    // Project drill-down

    std::vector<std::string> projectList;

    int projectDrillIdx = -1;  // selected project index in projectList, -1 = show list



    // Insertion position for a/i key (index in tasks array, -1 = append)

    int insertAfter = -1;



    // project selector index (for 'j' type field editing)

    int projectSelIdx = -1;



    // picker popup

    std::vector<PickerOpt> pickerOpts;

    int pickerSel = 0;

    int pickerField = -1;  // which detail field the picker is for



    // calendar popup

    int calYear = 2026;

    int calMonth = 1;

    int calDay = 1;

    int calSelDay = 1;



    // help dialog

    int helpScroll = 0;

    int helpPrevMode = M_BROWSE;



    // note editor

    std::vector<std::string> noteLines;

    int noteRow = 0;    // cursor line

    int noteCol = 0;    // cursor byte offset in line

    int noteScroll = 0;

    std::vector<VRow> noteVrows;

    bool noteVrowsDirty = true;



    // context/tag management

    std::vector<std::string> contextList;

    std::vector<std::string> tagList;

    std::string filterContext;

    std::vector<std::string> filterTags;

    int ctxMgrSel = 0;

    int tagMgrSel = 0;

    std::set<int> pickerToggled;  // for multi-select tag picker

    std::string renameTargetContext;

    std::string renameTargetTag;



    // summary dialog

    int summaryScroll = 0;
    int summaryPrevMode = M_BROWSE;



    // confirm dialog

    std::string confirmMsg;

    int confirmIdx = -1;

    std::vector<int> confirmIdxs;  // 批量删除确认的任务下标(M_CONFIRM 用)



    // multi-select (Shift+↑↓ 连续多选)

    std::set<std::string> multiSel;  // 选中的任务 id 集合

    int multiAnchor = -1;            // 选择锚点(g.filtered 显示下标), -1=未激活



    // project rename target

    std::string renameTargetProject;

    // 长按列表项弹出的编辑菜单（M_ITEM_MENU）。菜单里的动作都直接复用
    // 键盘路径（g.sel 已经指向被长按的那一行，和按 r/d 等价）。
    int itemMenuSel = 0;      // 菜单选中项



    // fold state (indices into g_gtdTree that are collapsed)

    std::set<int> foldedNodes;



    // archive manager

    std::vector<std::string> archiveMonths;  // "YYYY-MM" list

    std::vector<int> archiveCounts;          // task count per month

    std::vector<JsonValue> archiveTasks;     // tasks of selected month (read-only browse)

    int archiveSel = 0;

    int archiveScroll = 0;

    int archiveViewSel = 0;   // index in archiveTasks when browsing

    int archiveViewScroll = 0;

    bool archiveBrowsing = false;  // false = month list, true = task list

    std::string archiveViewMonth;  // currently viewed month

} g;

// 仅列表浏览模式启用物理按键导航快捷键,避免与文本输入冲突
bool screen_gtd_accept_physical_buttons() { return g.mode == M_BROWSE; }

// 项目标签顶层(项目选择列表)
bool screen_gtd_in_project_list() {
    return g.mode == M_BROWSE && g.view == V_PROJECT && g.projectDrillIdx < 0;
}

// 进度百分比(0-100) → 图标(U+E004=0%, U+E005..U+E00C=1/8..8/8)
static const char *progressIconStr(int pct) {
    static const char *icons[9] = { "\xEE\x80\x84", "\xEE\x80\x85", "\xEE\x80\x86", "\xEE\x80\x87",
                                    "\xEE\x80\x88", "\xEE\x80\x89", "\xEE\x80\x8A", "\xEE\x80\x8B",
                                    "\xEE\x80\x8C" };
    if (pct <= 0) return icons[0];
    int lvl = (pct * 2 + 24) / 25;
    if (lvl > 8) lvl = 8;
    return icons[lvl];
}

// GTD 状态栏右侧: 分隔符图标 + 当前日期(月-日) + 设备电量 (日期左边始终有分隔符)
static void gtdStatusRight(char *buf, size_t sz) {
    time_t now; time(&now);
    struct tm *tm = localtime(&now);
    char date[8];
    strftime(date, sizeof(date), "%m-%d", tm);
    // 与写作模式状态栏统一：电池图标 + 百分比数字（用户 2026-10-03 要求回到数字）。
    std::string bt = battery_status_text();
    if (!bt.empty())
        snprintf(buf, sz, "\xEE\x80\x83%s %s", date, bt.c_str());
    else
        snprintf(buf, sz, "\xEE\x80\x83%s", date);
}



// ── Tree view (for Project tab drill-down) ──────────────────────────────

struct GtdTreeItem {

    int taskIdx;

    int depth;

    bool isLast;

    std::vector<bool> ancLast; // ancestor-is-last at each level 0..depth-1

};

static std::vector<GtdTreeItem> g_gtdTree;
static std::vector<int> g_visibleTreeIdx; // indices into g_gtdTree, after fold filtering



static void gtdTreeAddChildren(const JsonValue &tasks, int startFi,

    int depth, const std::vector<bool> &ancLast,

    const std::vector<int> &scope, std::vector<bool> &used)

{

    if (depth > 50) return;

    std::string parentId = tasks[scope[startFi]]["id"].asString();



    std::vector<int> childFis;

    for (int fi = 0; fi < (int)scope.size(); fi++) {

        if (used[fi]) continue;

        if (tasks[scope[fi]]["parent"].asString() == parentId)

            childFis.push_back(fi);

    }



    for (size_t ci = 0; ci < childFis.size(); ci++) {

        int fi = childFis[ci];

        used[fi] = true;



        GtdTreeItem item;

        item.taskIdx = scope[fi];

        item.depth = depth;

        item.isLast = (ci == childFis.size() - 1);

        item.ancLast = ancLast;

        g_gtdTree.push_back(item);



        std::vector<bool> childAnc = ancLast;

        childAnc.push_back(item.isLast);

        gtdTreeAddChildren(tasks, fi, depth + 1, childAnc, scope, used);

    }

}



static void buildGtdTree() {

    g_gtdTree.clear();

    if (g.view != V_PROJECT || g.projectDrillIdx < 0) return;

    auto &tasks = g.data["tasks"];

    if (!tasks.isArray() || g.filtered.empty()) return;



    // Collect IDs in the project scope for parent-exists checks

    std::set<std::string> idsInScope;

    for (int fi : g.filtered) {

        idsInScope.insert(tasks[fi]["id"].asString());

    }



    // Copy filtered list as a scope for tree building

    std::vector<int> scope = g.filtered;

    std::vector<bool> used(scope.size(), false);



    // Pass 1: add roots (empty parent, or parent not in scope)

    for (int fi = 0; fi < (int)scope.size(); fi++) {

        if (used[fi]) continue;

        std::string pid = tasks[scope[fi]]["parent"].asString();

        if (pid.empty() || idsInScope.find(pid) == idsInScope.end()) {

            used[fi] = true;

            GtdTreeItem item;

            item.taskIdx = scope[fi];

            item.depth = 0;

            item.isLast = true; // tentative

            g_gtdTree.push_back(item);

        }

    }



    // Pass 2: add children recursively for each root

    // Re-walk the tree to fix isLast and add children properly

    // (Simpler: rebuild from scratch using the recursive approach)

    g_gtdTree.clear();



    // Rebuild properly with recursive children

    for (int fi = 0; fi < (int)scope.size(); fi++) {

        if (used[fi]) { // still true from Pass 1 for those we marked

            // These are roots — process them

        }

    }



    // Start fresh

    std::vector<bool> used2(scope.size(), false);

    std::vector<int> roots;

    for (int fi = 0; fi < (int)scope.size(); fi++) {

        std::string pid = tasks[scope[fi]]["parent"].asString();

        if (pid.empty() || idsInScope.find(pid) == idsInScope.end()) {

            roots.push_back(fi);

        }

    }

    // Determine isLast for each root

    for (size_t ri = 0; ri < roots.size(); ri++) {

        int fi = roots[ri];

        used2[fi] = true;

        GtdTreeItem item;

        item.taskIdx = scope[fi];

        item.depth = 0;

        item.isLast = (ri == roots.size() - 1);

        g_gtdTree.push_back(item);



        std::vector<bool> childAnc;

        childAnc.push_back(item.isLast);

        gtdTreeAddChildren(tasks, fi, 1, childAnc, scope, used2);

    }



    // Pass 3: any remaining orphans (circular refs etc.) as roots

    for (int fi = 0; fi < (int)scope.size(); fi++) {

        if (used2[fi]) continue;

        used2[fi] = true;

        GtdTreeItem item;

        item.taskIdx = scope[fi];

        item.depth = 0;

        item.isLast = true;

        g_gtdTree.push_back(item);

    }

}



// ── Filter helpers ───────────────────────────────────────────────────────

static bool taskMatchesView(const JsonValue &task, int view) {

    std::string status = task["status"].asString("todo");

    if (view == V_INBOX)     return status == "todo";

    if (view == V_NEXT)      return status == "doing";

    if (view == V_WAITING)   return status == "waiting";

    if (view == V_PROJECT)   return !task["project"].asString().empty();

    if (view == V_COMPLETED) return status == "done";

    return false;

}



static bool isInProjectList() { return g.view == V_PROJECT && g.projectDrillIdx < 0; }

// ── Multi-select (Shift+↑↓) ────────────────────────────────────────────
static bool hasMultiSel() { return !g.multiSel.empty(); }

static void clearMultiSel() {
    g.multiSel.clear();
    g.multiAnchor = -1;
}

// 由锚点..光标的连续区间从 g.filtered 重建选中任务 id 集合
static void rebuildMultiSel() {
    g.multiSel.clear();
    if (g.multiAnchor < 0) return;
    int a = std::min(g.multiAnchor, g.sel);
    int b = std::max(g.multiAnchor, g.sel);
    auto &tasks = g.data["tasks"];
    for (int i = a; i <= b && i < (int)g.filtered.size(); i++) {
        g.multiSel.insert(tasks[g.filtered[i]]["id"].asString());
    }
}

// displayIdx 位置的条目是否处于多选中
static bool isMultiSelected(int displayIdx) {
    if (g.multiSel.empty()) return false;
    auto &tasks = g.data["tasks"];
    if (displayIdx < 0 || displayIdx >= (int)g.filtered.size()) return false;
    return g.multiSel.count(tasks[g.filtered[displayIdx]]["id"].asString()) > 0;
}

static void rebuildFilter() {

    g.filtered.clear();

    auto &tasks = g.data["tasks"];

    if (!tasks.isArray()) return;

    for (int i = 0; i < (int)tasks.size(); i++) {

        auto &t = tasks[i];

        if (!taskMatchesView(t, g.view)) continue;

        if (g.view == V_PROJECT && g.projectDrillIdx >= 0) {

            std::string proj = t["project"].asString();

            if (proj.empty() || proj != g.projectList[g.projectDrillIdx])

                continue;

        }

        if (!g.filterText.empty()) {

            std::string title = t["title"].asString();

            std::string note  = t["note"].asString();

            if (title.find(g.filterText) == std::string::npos &&

                note.find(g.filterText) == std::string::npos)

                continue;

        }

        if (!g.filterContext.empty()) {

            if (t["context"].asString() != g.filterContext)

                continue;

        }

        if (!g.filterTags.empty()) {

            auto &tt = t["tags"];

            bool hasAll = true;

            for (auto &ft : g.filterTags) {

                bool found = false;

                if (tt.isArray()) {

                    for (int j = 0; j < (int)tt.size(); j++) {

                        if (tt[j].asString() == ft) { found = true; break; }

                    }

                }

                if (!found) { hasAll = false; break; }

            }

            if (!hasAll) continue;

        }

        g.filtered.push_back(i);

    }

    if (g.sel >= (int)g.filtered.size()) g.sel = (int)g.filtered.size() - 1;

    if (g.sel < 0) g.sel = 0;



    // Build tree view and reorder filtered to tree order for project drill-down

    if (g.view == V_PROJECT && g.projectDrillIdx >= 0) {

        buildGtdTree();

        // Apply fold: build visible tree index list (skipping children of folded nodes)
        g_visibleTreeIdx.clear();
        std::set<int> hiddenByFold;
        for (int fi : g.foldedNodes) {
            if (fi < 0 || fi >= (int)g_gtdTree.size()) continue;
            int foldDepth = g_gtdTree[fi].depth;
            for (int j = fi + 1; j < (int)g_gtdTree.size(); j++) {
                if (g_gtdTree[j].depth <= foldDepth) break;
                hiddenByFold.insert(j);
            }
        }
        for (int i = 0; i < (int)g_gtdTree.size(); i++) {
            if (!hiddenByFold.count(i))
                g_visibleTreeIdx.push_back(i);
        }

        g.filtered.clear();
        for (int vi : g_visibleTreeIdx)
            g.filtered.push_back(g_gtdTree[vi].taskIdx);

        if (g.sel >= (int)g.filtered.size()) g.sel = (int)g.filtered.size() - 1;

        if (g.sel < 0) g.sel = 0;

    }

}



static void buildProjectList() {

    g.projectList.clear();

    // Add projects from stored "projects" array

    auto &projs = g.data["projects"];

    if (projs.isArray()) {

        for (int i = 0; i < (int)projs.size(); i++) {

            std::string name = projs[i].asString();

            if (name.empty()) continue;

            bool dup = false;

            for (auto &p : g.projectList) if (p == name) { dup = true; break; }

            if (!dup) g.projectList.push_back(name);

        }

    }

    // Add projects derived from tasks

    auto &tasks = g.data["tasks"];

    if (tasks.isArray()) {

        for (int i = 0; i < (int)tasks.size(); i++) {

            std::string proj = tasks[i]["project"].asString();

            if (proj.empty()) continue;

            bool dup = false;

            for (auto &p : g.projectList) if (p == proj) { dup = true; break; }

            if (!dup) g.projectList.push_back(proj);

        }

    }

    std::sort(g.projectList.begin(), g.projectList.end());

}

// 物理按键双击 BOOT: 项目树内返回项目选择菜单; 其他视图无动作
void screen_gtd_physical_double_boot() {
    if (g.mode != M_BROWSE) return;
    if (g.view == V_PROJECT && g.projectDrillIdx >= 0) {
        g.projectDrillIdx = -1;
        g.sel = 0; g.scroll = 0;
        g.foldedNodes.clear();
        clearMultiSel();
        buildProjectList();
        rebuildFilter();
    }
}



static void buildContextList() {

    g.contextList.clear();

    auto &ctxs = g.data["contexts"];

    if (ctxs.isArray()) {

        for (int i = 0; i < (int)ctxs.size(); i++) {

            std::string name = ctxs[i].asString();

            if (name.empty()) continue;

            bool dup = false;

            for (auto &c : g.contextList) if (c == name) { dup = true; break; }

            if (!dup) g.contextList.push_back(name);

        }

    }

    auto &tasks = g.data["tasks"];

    if (tasks.isArray()) {

        for (int i = 0; i < (int)tasks.size(); i++) {

            std::string ctx = tasks[i]["context"].asString();

            if (ctx.empty()) continue;

            bool dup = false;

            for (auto &c : g.contextList) if (c == ctx) { dup = true; break; }

            if (!dup) g.contextList.push_back(ctx);

        }

    }

    std::sort(g.contextList.begin(), g.contextList.end());

}



static void buildTagList() {

    g.tagList.clear();

    auto &tags = g.data["tags"];

    if (tags.isArray()) {

        for (int i = 0; i < (int)tags.size(); i++) {

            std::string name = tags[i].asString();

            if (name.empty()) continue;

            bool dup = false;

            for (auto &t : g.tagList) if (t == name) { dup = true; break; }

            if (!dup) g.tagList.push_back(name);

        }

    }

    auto &tasks = g.data["tasks"];

    if (tasks.isArray()) {

        for (int i = 0; i < (int)tasks.size(); i++) {

            auto &tt = tasks[i]["tags"];

            if (tt.isArray()) {

                for (int j = 0; j < (int)tt.size(); j++) {

                    std::string name = tt[j].asString();

                    if (name.empty()) continue;

                    bool dup = false;

                    for (auto &t : g.tagList) if (t == name) { dup = true; break; }

                    if (!dup) g.tagList.push_back(name);

                }

            }

        }

    }

    std::sort(g.tagList.begin(), g.tagList.end());

}



// ── Data I/O ─────────────────────────────────────────────────────────────

static void loadData() {

    auto v = JsonValue::loadFromFile(DATA_FILE);

    if (v.isNull() || !v.has("tasks") || !v["tasks"].isArray()) {

        g.data = JsonValue::object();

        g.data.set("tasks", JsonValue::array());

    } else {

        g.data = v;

        // Purge null-type tasks from old bug

        auto &tasks = g.data["tasks"];

        int write = 0;

        for (int i = 0; i < (int)tasks.size(); i++) {

            if (!tasks[i].isNull())

                tasks.elements[write++] = tasks[i];

        }

        tasks.elements.resize(write);

    }

    if (!g.data.has("projects") || !g.data["projects"].isArray())

        g.data.set("projects", JsonValue::array());

    if (!g.data.has("contexts") || !g.data["contexts"].isArray())

        g.data.set("contexts", JsonValue::array());

    if (!g.data.has("tags") || !g.data["tags"].isArray())

        g.data.set("tags", JsonValue::array());

    g.view = 0; g.sel = 0; g.scroll = 0;

    rebuildFilter();

}



static void saveData() {

    if (!JsonValue::saveToFile(DATA_FILE, g.data)) {

        // Save failed — try ensuring directory exists and retry once

        mkdir(DATA_DIR, 0755);

        JsonValue::saveToFile(DATA_FILE, g.data);

    }

}



// ── Auto-archive ─────────────────────────────────────────────────────────

static std::string currentMonthStr() {
    time_t now; time(&now); struct tm *tm = localtime(&now);
    char buf[32];
    snprintf(buf, sizeof(buf), "%04d-%02d", tm->tm_year + 1900, tm->tm_mon + 1);
    return buf;
}

static void autoArchive() {
    std::string curMonth = currentMonthStr();
    std::string lastArchive = g.data["lastArchive"].asString();

    // Already archived this month
    if (lastArchive == curMonth) return;

    mkdir(ARCHIVE_DIR, 0755);

    auto &tasks = g.data["tasks"];
    if (!tasks.isArray()) return;

    // Group completed tasks by their completed-month
    // key = "YYYY-MM", value = array of task indices (in reverse for safe erase)
    std::map<std::string, std::vector<int>> byMonth;
    for (int i = 0; i < (int)tasks.size(); i++) {
        if (tasks[i]["status"].asString("todo") != "done") continue;
        std::string completed = tasks[i]["completed"].asString();
        if (completed.empty()) continue;
        // completed format: "YYYY-MM-DD"
        if (completed.length() < 7) continue;
        std::string month = completed.substr(0, 7);
        // Only archive tasks from previous months or earlier
        if (month >= curMonth) continue;
        byMonth[month].push_back(i);
    }

    bool anyArchived = false;
    for (auto &kv : byMonth) {
        std::string month = kv.first;
        std::vector<int> &indices = kv.second;

        // Load existing archive file (if any) and merge
        std::string arcPath = std::string(ARCHIVE_DIR) + "/" + month + ".json";
        JsonValue arcData = JsonValue::loadFromFile(arcPath);
        if (arcData.isNull() || !arcData.has("tasks") || !arcData["tasks"].isArray()) {
            arcData = JsonValue::object();
            arcData.set("tasks", JsonValue::array());
            arcData.set("archiveDate", month);
        }
        auto &arcTasks = arcData["tasks"];

        // Append archived tasks
        for (int idx : indices)
            arcTasks.pushBack(tasks[idx]);

        JsonValue::saveToFile(arcPath, arcData);
        anyArchived = true;
    }

    // Remove archived tasks from main data (erase from back to front)
    std::vector<int> allToRemove;
    for (auto &kv : byMonth)
        for (int idx : kv.second)
            allToRemove.push_back(idx);
    std::sort(allToRemove.rbegin(), allToRemove.rend());
    for (int idx : allToRemove)
        tasks.elements.erase(tasks.elements.begin() + idx);

    g.data.set("lastArchive", curMonth);
    if (anyArchived) saveData();
}



// ── ID generator ─────────────────────────────────────────────────────────

static std::string makeId() {

    time_t now; time(&now); struct tm *tm = localtime(&now);

    char buf[32];

    static int seq = 0;

    snprintf(buf, sizeof(buf), "%02d%02d%02d_%02d%02d_%d",

             tm->tm_year % 100, tm->tm_mon + 1, tm->tm_mday,

             tm->tm_hour, tm->tm_min, seq++);

    return buf;

}

// ── Export markdown ──────────────────────────────────────────────────────

static std::string exportMD() {

    std::string md = "# GTD 任务列表\n\n";

    auto &tasks = g.data["tasks"];

    if (!tasks.isArray()) return md;

    for (int i = 0; i < (int)tasks.size(); i++) {

        auto &t = tasks[i];

        std::string status = t["status"].asString("todo");

        std::string pri = t["priority"].asString();

        std::string title = t["title"].asString();



        const char *mark = "[ ]";

        if (status == "doing") mark = "[→]";

        else if (status == "done") mark = "[✓]";

        else if (status == "waiting") mark = "[~]";



        md += "- " + std::string(mark) + " " + title;

        if (!pri.empty()) md += " **" + pri + "**";

        md += "\n";

    }

    return md;

}



// ── UI drawing ───────────────────────────────────────────────────────────



static void drawIMEStatus() {
    drawIMEUIWithStatusBar();
}

// 标签栏格子宽度：曾是"按标题文字宽等比分配"（三字标题挤、两字标题松）。换成图标
// 之后各格语义宽度相同，直接五等分，末格吃掉取整误差正好铺满；点按命中和绘制查
// 同一张宽度表，所见即所点。
static void gtdTabWidths(int *out) {
    const int base = SCREEN_W / V_COUNT;
    int acc = 0;
    for (int v = 0; v < V_COUNT; v++) {
        int w = (v == V_COUNT - 1) ? SCREEN_W - acc : base;
        if (w < 1) w = 1;
        out[v] = w;
        acc += w;
    }
}

static int gtdTabAtX(int x) {
    int w[V_COUNT];
    gtdTabWidths(w);
    int acc = 0;
    for (int v = 0; v < V_COUNT; v++) {
        acc += w[v];
        if (x < acc) return v;
    }
    return V_COUNT - 1;
}

// 标签栏几何全部走 tab_icons.h 的共用常量（TAB_ICON_INSET / TAB_ICON_PX /
// TAB_BOX_PAD / TAB_BAND_BOTTOM），和阅读模式的根标签栏是同一套数值——两个模式
// 切过去，图标大小和离顶边的距离都一样。放大图标后标签栏整体变高，列表起点跟着
// gtdFirstRowY() 一起下移。
static void drawTabBar() {

    int tws[V_COUNT];
    gtdTabWidths(tws);

    uint8_t *fb = u8g2_GetBufferPtr(g_u8g2);
    const int box = TAB_ICON_PX + 2 * TAB_BOX_PAD;

    int x = 0;

    for (int v = 0; v < V_COUNT; v++) {

        const int tw = tws[v];

        const bool act = (v == g.view);

        // 活动标签：黑底 + 反白图标。底色的宽高只按图标算（原来铺满一整格，
        // 5 格铺满看着太闷），位置和阅读模式那边一样是居中一个方块。
        // 底色必须先填黑，invert=true 只画白像素、不会自己去反底。
        if (act) {

            u8g2_SetDrawColor(g_u8g2, 0);

            u8g2_DrawBox(g_u8g2, x + (tw - box) / 2, TAB_ICON_INSET - TAB_BOX_PAD, box, box);

        }

        if (fb)
            icon_font_draw_sized(fb, x + (tw - TAB_ICON_PX) / 2, TAB_ICON_INSET, TAB_ICON_PX,
                                 TAB_ICON_PX, kGtdTabIcons[v], act, TAB_ICON_PX);

        x += tw;

    }

    u8g2_SetDrawColor(g_u8g2, 0);

    u8g2_DrawHLine(g_u8g2, 0, TAB_BAND_BOTTOM, SCREEN_W);

}

// ── 触摸交互 ────────────────────────────────────────────────────────────
// 计划模式原本只有键盘路径：触屏点按一律退化成 Enter、长按退化成 Esc，
// 除了上下滑（整页→单步）几乎没法用。这里把点按坐标翻译成具体动作：
// 切标签、点列表行、点详情字段、点选择器选项，以及状态栏上方常驻的悬浮「+」。
//
// 坐标 → 键码的契约（gtdKeyFromTap，返回值就是要喂给主处理函数的键）：
//   -1  不是本层认领的点按 → 保持原样，仍旧按 Enter 语义走
//    0  已就地处理完毕（改了状态，只需重绘）
//   其它 用这个键继续走**原有的键盘逻辑**（'a' 新建、'\n' 确认/进入…），
//        这样各处行为与按键完全一致，不会出现两套实现跑偏。

// 悬浮「+」新建键：贴在状态栏上方右侧。列表下边界主动让出一条带（见
// gtdListMaxY），所以它盖不到正文，看上去是浮在空白上。
static int gtdFabSize() { return FONT_H + 6; }
static int gtdFabX() { return SCREEN_W - gtdFabSize() - 12; }
static int gtdFabY() { return STATUS_BAR_Y - gtdFabSize() - 8; }
// 列表可用下边界（给悬浮键让位）。
// ── 方向浮动按钮（项目详情列表专用）────────────────────────────────────
// 项目下钻后的任务树里，键盘上调整顺序/层级的四个键（j 上移 / k 下移 / h 升级 /
// l 降级）触屏按不到，于是在左下角浮一排方向键：←升级 →降级 ↑上移 ↓下移。
// 点中就是喂对应的键码，走**同一条**重排逻辑（见 gtdKeyFromTap 的注释契约），
// 不另写一套实现。位置与右下角的「+」同一条留白带，列表下边界让出这一条，
// 所以它盖不到正文行。
#define GTDPAD_BTN (FONT_H + 10)   // 单键边长（比「+」大一圈，手指好点）
#define GTDPAD_GAP 6
static bool gtdDirPadShown() { return g.view == V_PROJECT && g.projectDrillIdx >= 0; }
static int gtdDirPadY() { return STATUS_BAR_Y - 8 - GTDPAD_BTN; }
static int gtdDirPadX() { return 4; }
// 四个方向键的字形朝向与对应键码：左/右 = 层级，上/下 = 顺序。
// 与列表里从左到右的排布一致（先层级后顺序）。
static const int kDirPadArrow[4] = {2, 3, 0, 1};   // 2=← 3=→ 0=↑ 1=↓
static const int kDirPadKey[4]   = {'h', 'l', 'j', 'k'};

static int gtdListMaxY() {
    // 项目详情列表里左下角多一排方向键（比「+」高 4px），列表下边界跟着上移，
    // 免得最后一行压在按钮上。
    if (gtdDirPadShown()) return gtdDirPadY() - 4;
    return gtdFabY() - 4;
}

static void drawGtdFab() {
    int s = gtdFabSize();
    int x = gtdFabX(), y = gtdFabY();
    int cx = x + s / 2, cy = y + s / 2, arm = s / 3;
    if (arm < 4) arm = 4;
    u8g2_SetDrawColor(g_u8g2, 0);
    u8g2_DrawBox(g_u8g2, x, y, s, s);
    u8g2_SetDrawColor(g_u8g2, 1);
    u8g2_DrawBox(g_u8g2, cx - 2, cy - arm / 2, 5, arm + (arm & 1));      // 竖笔
    u8g2_DrawBox(g_u8g2, cx - arm / 2, cy - 2, arm + (arm & 1), 5);      // 横笔
    u8g2_SetDrawColor(g_u8g2, 0);
}

static bool gtdFabHit(int x, int y) {
    int s = gtdFabSize();
    return x >= gtdFabX() && x < gtdFabX() + s && y >= gtdFabY() && y < gtdFabY() + s;
}

// 悬浮「+子任务」键：和「+」同一排、紧贴它左边。样式是**空心方框 + 实心「+」**
// （「+」是实心黑底白加号），两者一眼分得开。只在项目详情列表里出现 —— 它喂的是
// 键盘上的 'i'（在**选中的那条**任务下面挂子任务），项目列表层没有"选中任务"可言。
static int gtdSubFabX() { return gtdFabX() - gtdFabSize() - 8; }
static int gtdSubFabY() { return gtdFabY(); }

static bool gtdSubFabHit(int x, int y) {
    if (!gtdDirPadShown()) return false;
    int s = gtdFabSize();
    return x >= gtdSubFabX() && x < gtdSubFabX() + s && y >= gtdSubFabY() && y < gtdSubFabY() + s;
}

static void drawGtdSubFab() {
    if (!gtdDirPadShown()) return;
    int s = gtdFabSize();
    int x = gtdSubFabX(), y = gtdSubFabY();
    int cx = x + s / 2, cy = y + s / 2, arm = s / 3;
    if (arm < 4) arm = 4;
    const int t = 2;                                    // 边框粗细
    u8g2_SetDrawColor(g_u8g2, 1);
    u8g2_DrawBox(g_u8g2, x, y, s, s);                   // 白底（空心）
    u8g2_SetDrawColor(g_u8g2, 0);
    u8g2_DrawBox(g_u8g2, x, y, s, t);                   // 上边
    u8g2_DrawBox(g_u8g2, x, y + s - t, s, t);           // 下边
    u8g2_DrawBox(g_u8g2, x, y, t, s);                   // 左边
    u8g2_DrawBox(g_u8g2, x + s - t, y, t, s);           // 右边
    u8g2_DrawBox(g_u8g2, cx - 2, cy - arm / 2, 5, arm + (arm & 1));      // 竖笔
    u8g2_DrawBox(g_u8g2, cx - arm / 2, cy - 2, arm + (arm & 1), 5);      // 横笔
    u8g2_SetDrawColor(g_u8g2, 0);
}

// 命中第几个键（0..3）；未命中返回 -1。
static int gtdDirPadHit(int x, int y) {
    if (!gtdDirPadShown()) return -1;
    int y0 = gtdDirPadY();
    if (y < y0 || y >= y0 + GTDPAD_BTN) return -1;
    int x0 = gtdDirPadX();
    if (x < x0) return -1;
    int i = (x - x0) / (GTDPAD_BTN + GTDPAD_GAP);
    if (i < 0 || i > 3) return -1;
    int bx = x0 + i * (GTDPAD_BTN + GTDPAD_GAP);
    if (x >= bx + GTDPAD_BTN) return -1;   // 落在两个键之间的缝里
    return i;
}

// 白色实心箭头：逐行加宽的横条堆出三角头 + 一根细杆（shim 只有方块/线，
// 没有三角形基元）。dir: 0=上 1=下 2=左 3=右，箭头以 (cx,cy) 为中心、长 size。
static void gtdDrawArrow(int cx, int cy, int size, int dir) {
    const int half = size / 4;         // 三角底半宽
    const int head = size / 2;         // 三角头长度
    const int shaft = (size / 4) | 1;  // 杆宽（奇数便于居中）
    const int a0 = -(size / 2);
    for (int i = 0; i < size; i++) {
        int w = (i < head) ? (2 * half * (i + 1) / head) : shaft;
        if (w < 1) w = 1;
        int off = a0 + i;
        if (dir == 0)      u8g2_DrawBox(g_u8g2, cx - w / 2, cy + off, w, 1);
        else if (dir == 1) u8g2_DrawBox(g_u8g2, cx - w / 2, cy - off, w, 1);
        else if (dir == 2) u8g2_DrawBox(g_u8g2, cx + off, cy - w / 2, 1, w);
        else               u8g2_DrawBox(g_u8g2, cx - off, cy - w / 2, 1, w);
    }
}

// 画这一排方向键（黑底白箭头，与「+」同一套观感）。只在项目详情列表里出现。
static void drawGtdDirPad() {
    if (!gtdDirPadShown()) return;
    int s = GTDPAD_BTN;
    int y = gtdDirPadY();
    int asz = s - 14;
    if (asz < 10) asz = 10;
    if (asz > s - 8) asz = s - 8;
    for (int i = 0; i < 4; i++) {
        int x = gtdDirPadX() + i * (s + GTDPAD_GAP);
        u8g2_SetDrawColor(g_u8g2, 0);
        u8g2_DrawBox(g_u8g2, x, y, s, s);
        u8g2_SetDrawColor(g_u8g2, 1);
        gtdDrawArrow(x + s / 2, y + s / 2, asz, kDirPadArrow[i]);
        u8g2_SetDrawColor(g_u8g2, 0);
    }
}

// 列表首行的文字基线（drawList 的三处列表分支都用同一个起点）。
// 列表首行基线。分隔线在 TAB_BAND_BOTTOM，往下留 LINE_SPACING-1 再起第一行——
// 这样行顶离分隔线的空档和标签栏没放大之前完全一样（行距、行内基线关系都不动），
// 只是整块跟着标签栏一起下移。
static int gtdFirstRowY() { return TAB_BAND_BOTTOM + LINE_SPACING - 1; }

// 点按 y → 行号。每行的命中带就是一格行距，从首行基线往上 ascent 处开始。
static int gtdRowAtY(int y0, int ty, int rows) {
    if (rows <= 0) return -1;
    int top0 = y0 - g_font.ascent();
    if (ty < top0) return -1;
    int i = (ty - top0) / LINE_SPACING;
    return (i >= 0 && i < rows) ? i : -1;
}

// 当前列表的行数（与 drawList 的三处分支对应）。
static int gtdListRows() {
    if (isInProjectList()) return (int)g.projectList.size();
    if (g.view == V_PROJECT && g.projectDrillIdx >= 0) return (int)g_visibleTreeIdx.size();
    return (int)g.filtered.size();
}

// ── 编辑态虚拟键盘 ──────────────────────────────────────────────────────
// 与写作模式共用同一套键盘(editor_vk)：没连蓝牙键盘时自动弹出，连上自动收起。
// 列表浏览态不弹（整屏要留给任务），只有要往输入框里打字的时候才占下半屏。
static bool gtdVkEditing() {
    switch (g.mode) {
        case M_ADD: case M_RENAME:
        case M_ADD_PROJECT: case M_RENAME_PROJECT:
        case M_ADD_CONTEXT: case M_ADD_TAG:
        case M_RENAME_CONTEXT: case M_RENAME_TAG:
        case M_EDIT_FIELD: case M_EDIT_NOTE:
            return true;
        // 筛选是在列表上直接打字；项目列表/项目树那两个分支不画键盘，也就别拦点按。
        case M_FILTER:
            return g.view != V_PROJECT;
        default:
            return false;
    }
}

// 键盘此刻确实占着屏幕下半部分吗（编辑态 + 已展开）。
static bool gtdVkActive() { return gtdVkEditing() && editorVkVisible(); }

bool screen_gtd_typing_mode() { return gtdVkEditing(); }

// 同上，供 main 的刷屏策略使用。
bool screen_gtd_vk_up() { return gtdVkActive(); }

// 输入区下边界：键盘弹起时裁到键盘面板顶边，否则是状态栏上方。
static int gtdInputBottomY() {
    int b = g_ime.composing() ? imeStatusPanelTopY() : STATUS_Y;
    if (gtdVkActive()) {
        int t = editorVkTop();
        if (t < b) b = t;
    }
    return b;
}

// 列表可视区的下边界：悬浮「+」上方，输入法面板弹起时再让位给它。
static int gtdListBottomY() {
    int b = gtdListMaxY();
    if (g_ime.composing()) {
        int t = imeStatusPanelTopY();
        if (t < b) b = t;
    }
    // 筛选是在列表上直接打字(不进子界面)，键盘弹起时列表要让出它的高度，
    // 还得再空出一行给"筛选: xxx"那行提示。
    if (g.mode == M_FILTER && gtdVkActive()) {
        int t = editorVkTop() - LINE_SPACING;
        if (t < b) b = t;
    }
    return b;
}

// 当前列表的一屏行数（与 drawList 的算法保持一致，供滑动整页用）。
static int gtdListVisibleRows() {
    int vis = (gtdListBottomY() - gtdFirstRowY() + LINE_SPACING - 1) / LINE_SPACING;
    return vis < 1 ? 1 : vis;
}

// 绘制时要画的行数：滚动停在小数偏移上时，最下面会多露出小半行，得多画一行，
// 否则滚到底部会缺一条。
static int gtdListDrawRows() {
    return gtdListVisibleRows() + (g.scrollFrac > 0 ? 1 : 0);
}

// 切换标签页（Tab 键与点击标签栏共用；翻页时把项目下钻复位到项目列表）。
static void gtdSetView(int v) {
    v = ((v % V_COUNT) + V_COUNT) % V_COUNT;
    if (v == g.view && g.projectDrillIdx < 0) return;
    g.view = v;
    g.sel = 0; g.scroll = 0; g.scrollFrac = 0;
    g.projectDrillIdx = -1;
    g.foldedNodes.clear();
    if (v == V_PROJECT) buildProjectList();
    rebuildFilter();
}

// ── 平滑滚动 + 右侧滚动条 ───────────────────────────────────────────────
// 手指拖动按像素滚动（g.scrollFrac 是窗口顶边相对整行的偏移，于是上下边缘会
// 露出半行，滚动是连续的而不是整行跳）。滚动条只在"装不下"时出现，滚动/拖动
// 后显示约 1.5 秒再自动隐去——隐去靠主循环的空闲重绘生效（ui_commit 比对快照，
// 没有变化不上屏，所以那次重绘正好把滚动条擦掉）。
static int64_t s_scrollBarUntil = 0;
// 刚才这次手势是拖动：抬手时还会再发一个"翻页键"，那一次要吃掉（已经按像素
// 滚过了，再跳一页会过头）。只吃触摸来的翻页键，键盘/遥控器的不受影响。
static bool s_dragConsumed = false;

static void gtdShowScrollBar() { s_scrollBarUntil = esp_timer_get_time() + 1500000; }

static void gtdScrollByPx(int dy) {
    if (dy == 0) return;
    int rows = gtdListRows();
    int vis = gtdListVisibleRows();
    int maxScroll = rows - vis;
    if (maxScroll < 0) maxScroll = 0;
    int maxPx = maxScroll * LINE_SPACING;
    int cur = g.scroll * LINE_SPACING + g.scrollFrac;
    // 手指下移(dy>0) = 内容跟着下移 = 看前面的行，窗口像素位置减小。
    int want = cur - dy;
    if (want < 0) want = 0;
    if (want > maxPx) want = maxPx;
    g.scroll = want / LINE_SPACING;
    g.scrollFrac = want % LINE_SPACING;
    // 选中行始终留在窗口内：Enter/详情作用的一定是看得见的那一行。
    if (g.sel < g.scroll) g.sel = g.scroll;
    if (g.sel > g.scroll + vis - 1) g.sel = g.scroll + vis - 1;
    if (g.sel < 0) g.sel = 0;
    gtdShowScrollBar();
}

// 整页翻（滑动抬手时的"快滑"）：保留一行重叠，翻页后仍有上下文。
static void gtdScrollByPage(int dir) {
    int vis = gtdListVisibleRows();
    int step = vis > 1 ? vis - 1 : 1;
    g.scrollFrac = 0;
    g.sel += dir * step;
    int rows = gtdListRows();
    int last = rows > 0 ? rows - 1 : 0;
    if (g.sel < 0) g.sel = 0;
    if (g.sel > last) g.sel = last;
    int maxScroll = rows - vis;
    if (maxScroll < 0) maxScroll = 0;
    int want = g.sel - (dir > 0 ? vis - 1 : 0);
    if (want < 0) want = 0;
    if (want > maxScroll) want = maxScroll;
    g.scroll = want;
    gtdShowScrollBar();
}

// 平滑滚动时，第 scroll 行会被顶到列表首行之上、露出上半截。这截越过了列表区，
// 会糊在标签栏上，所以画完行以后把标签栏以下、首行顶部以上的那条带擦白，
// 再把标签栏补画一遍——等于给列表加了个上裁剪。
static void gtdClipListTop() {
    if (g.scrollFrac <= 0) return;
    int cut = gtdFirstRowY() - g_font.ascent();
    if (cut <= 0) return;
    u8g2_SetDrawColor(g_u8g2, 1);
    u8g2_DrawBox(g_u8g2, 0, 0, SCREEN_W, cut);
    u8g2_SetDrawColor(g_u8g2, 0);
    drawTabBar();
}

// 右侧滚动条（画在列表区右边缘，避开内容）。
static void drawGtdScrollBar(int top, int bottom) {
    int rows = gtdListRows();
    int vis = gtdListVisibleRows();
    if (rows <= vis || bottom <= top + 8) return;
    if (esp_timer_get_time() > s_scrollBarUntil) return;
    int maxScroll = rows - vis;
    int x = SCREEN_W - 7, w = 4;
    int th = (int)((int64_t)(bottom - top) * vis / rows);
    if (th < FONT_H / 3) th = FONT_H / 3;
    int ty = top + (int)((int64_t)(bottom - top - th) * g.scroll / (maxScroll > 0 ? maxScroll : 1));
    u8g2_SetDrawColor(g_u8g2, 0);
    u8g2_DrawFrame(g_u8g2, x, top, w, bottom - top);
    u8g2_DrawBox(g_u8g2, x + 1, ty, w - 2, th);
    u8g2_SetDrawColor(g_u8g2, 0);
}

// 状态栏：右侧文字给键盘开关图标让位，画完状态栏再补画图标
// （图标必须在 ui_draw_status 之后，否则被状态栏的白底盖掉）。
static void gtdStatusBar(const char *left, const std::string &right) {
    std::string r = right;
    int slot = gtdVkEditing() ? editorVkIconSlotW() : 0;
    if (slot > 0) r = editorVkTruncateToWidth(r, SCREEN_W - slot - 16);
    // 与写作模式状态栏一致：rightReserve 把右对齐的文字从键盘开关图标底下挪出来，
    // 否则串尾的电池图标正好压在图标槽里（截断只管宽度，管不了右对齐的落点）。
    ui_draw_status(left, r.c_str(), slot);
    if (slot > 0) editorVkDrawIcon();
}

static const char *statusIcon(const std::string &s) {

    if (s == "doing")   return "[→]";

    if (s == "done")    return "[✓]";

    if (s == "waiting") return "[~]";

    return "[ ]";

}



static void drawDetail();  // forward declaration

static void drawList();    // forward declaration

static void drawHelp();    // forward declaration

// 触摸选区反白 + 按钮条（共享件 text_sel）。drawAdd/drawDetail 画完字段本体后调。
static void gtdFieldOverlay(const std::string &buf);



// ── Archive manager ──────────────────────────────────────────────────────

static void loadArchiveMonths() {
    g.archiveMonths.clear();
    g.archiveCounts.clear();
    DIR *dir = opendir(ARCHIVE_DIR);
    if (!dir) return;
    struct dirent *entry;
    while ((entry = readdir(dir)) != nullptr) {
        std::string name = entry->d_name;
        if (name.size() > 5 && name.substr(name.size() - 5) == ".json") {
            std::string month = name.substr(0, name.size() - 5);
            // Count tasks
            std::string path = std::string(ARCHIVE_DIR) + "/" + name;
            JsonValue data = JsonValue::loadFromFile(path);
            int count = 0;
            if (data.has("tasks") && data["tasks"].isArray()) count = (int)data["tasks"].size();
            g.archiveMonths.push_back(month);
            g.archiveCounts.push_back(count);
        }
    }
    closedir(dir);
    // Sort both vectors together (newest first)
    std::vector<int> order(g.archiveMonths.size());
    for (size_t i = 0; i < order.size(); i++) order[i] = i;
    std::sort(order.begin(), order.end(), [](int a, int b) {
        return g.archiveMonths[a] > g.archiveMonths[b];
    });
    std::vector<std::string> sm = g.archiveMonths;
    std::vector<int> sc = g.archiveCounts;
    for (size_t i = 0; i < order.size(); i++) {
        g.archiveMonths[i] = sm[order[i]];
        g.archiveCounts[i] = sc[order[i]];
    }
}

static void loadArchiveMonthTasks(const std::string &month) {
    g.archiveTasks.clear();
    std::string path = std::string(ARCHIVE_DIR) + "/" + month + ".json";
    JsonValue data = JsonValue::loadFromFile(path);
    if (!data.isNull() && data.has("tasks") && data["tasks"].isArray()) {
        auto &tasks = data["tasks"];
        for (int i = 0; i < (int)tasks.size(); i++)
            g.archiveTasks.push_back(tasks[i]);
    }
}

static void drawArchiveMgr() {
    ui_clear();

    if (!g.archiveBrowsing) {
        // Month list
        ui_draw_text(4, g_font.ascent(), "归档管理", false, true);
        u8g2_DrawHLine(g_u8g2, 0, FONT_H + 4, SCREEN_W);

        int y = FONT_H + 8 + LINE_SPACING;
        int vis = (STATUS_Y - y + LINE_SPACING - 1) / LINE_SPACING;
        if (vis < 1) vis = 1;
        if (g.archiveSel < g.archiveScroll) g.archiveScroll = g.archiveSel;
        if (g.archiveSel >= g.archiveScroll + vis) g.archiveScroll = g.archiveSel - vis + 1;

        if (g.archiveMonths.empty()) {
            ui_draw_text(8, y, "暂无归档");
        }
        for (int i = 0; i < vis && (g.archiveScroll + i) < (int)g.archiveMonths.size(); i++) {
            int mi = g.archiveScroll + i;
            bool sel = (mi == g.archiveSel);
            std::string month = g.archiveMonths[mi];
            int count = g.archiveCounts[mi];

            char buf[64];
            snprintf(buf, sizeof(buf), "%s  (%d项)", month.c_str(), count);
            ui_draw_text(8, y + i * LINE_SPACING, buf, sel);
        }

        char rbuf[40]; gtdStatusRight(rbuf, sizeof(rbuf));

        ui_draw_status("?:帮助|Enter展开 d:删除 Esc返回", rbuf);
    } else {
        // Task list for selected month
        char title[48];
        snprintf(title, sizeof(title), "归档: %s", g.archiveViewMonth.c_str());
        ui_draw_text(4, g_font.ascent(), title, false, true);
        u8g2_DrawHLine(g_u8g2, 0, FONT_H + 4, SCREEN_W);

        int y = FONT_H + 8 + LINE_SPACING;
        int vis = (STATUS_Y - y + LINE_SPACING - 1) / LINE_SPACING;
        if (vis < 1) vis = 1;
        if (g.archiveViewSel < g.archiveViewScroll) g.archiveViewScroll = g.archiveViewSel;
        if (g.archiveViewSel >= g.archiveViewScroll + vis) g.archiveViewScroll = g.archiveViewSel - vis + 1;

        for (int i = 0; i < vis && (g.archiveViewScroll + i) < (int)g.archiveTasks.size(); i++) {
            int ti = g.archiveViewScroll + i;
            bool sel = (ti == g.archiveViewSel);
            auto &t = g.archiveTasks[ti];
            std::string status = t["status"].asString("todo");
            std::string ttl = t["title"].asString();
            char buf[96];
            snprintf(buf, sizeof(buf), "%s %s", statusIcon(status), ttl.c_str());
            ui_draw_text(8, y + i * LINE_SPACING, buf, sel);
        }

        if (g.archiveTasks.empty()) ui_draw_text(8, y, "(空)");

        char sl[48];
        snprintf(sl, sizeof(sl), "?:帮助|%d项 Esc返回", (int)g.archiveTasks.size());
        char rbuf[40]; gtdStatusRight(rbuf, sizeof(rbuf));
        ui_draw_status(sl, rbuf);
    }

    drawIMEStatus(); ui_commit();
}



static void drawList() {

    ui_clear();

    drawTabBar();



    int y = gtdFirstRowY();



    // Project list (top level)

    if (g.view == V_PROJECT && g.projectDrillIdx < 0) {

        buildProjectList();

        int vis = gtdListDrawRows();

        if (vis < 1) vis = 1;

        if (g.sel < g.scroll) g.scroll = g.sel;

        if (g.sel >= g.scroll + vis) g.scroll = g.sel - vis + 1;

        for (int i = 0; i < vis && (g.scroll + i) < (int)g.projectList.size(); i++) {

            bool sel = (g.scroll + i == g.sel);

            char buf[64];

            snprintf(buf, sizeof(buf), "◆ %s", g.projectList[g.scroll + i].c_str());

            ui_draw_text(4, y + i * LINE_SPACING - g.scrollFrac, buf, sel);

        }

        gtdClipListTop();

        drawGtdScrollBar(y - g_font.ascent() - g.scrollFrac, gtdListBottomY());

        if (g.projectList.empty()) {

            ui_draw_text(4, y, "暂无项目 — 按n新建");

        }

        char sl[32];
        snprintf(sl, sizeof(sl), "?:帮助|%d个项目", (int)g.projectList.size());
        char rbuf[40]; gtdStatusRight(rbuf, sizeof(rbuf));
        ui_draw_status(sl, rbuf);

        drawIMEStatus();

        drawGtdFab();

        return;

    }



    // Project tree view (drill-down)

    if (g.view == V_PROJECT && g.projectDrillIdx >= 0) {

        if (g_gtdTree.empty()) {

            ui_draw_text(8, y, "此项目暂无任务 — 按a添加");

            char sl[96];

            snprintf(sl, sizeof(sl), "a:添加 Tab:切换");

            char rbuf[40]; gtdStatusRight(rbuf, sizeof(rbuf));

            ui_draw_status(sl, rbuf);

            drawIMEStatus();

            drawGtdFab();

            drawGtdSubFab();

            return;

        }

        auto &tasks = g.data["tasks"];

        int vis = gtdListDrawRows();

        if (vis < 1) vis = 1;

        if (g.sel < g.scroll) g.scroll = g.sel;

        if (g.sel >= g.scroll + vis) g.scroll = g.sel - vis + 1;



        for (int i = 0; i < vis && (g.scroll + i) < (int)g_visibleTreeIdx.size(); i++) {

            int treeIdx = g_visibleTreeIdx[g.scroll + i];
            auto &ti = g_gtdTree[treeIdx];

            bool sel = (g.scroll + i == g.sel);

            auto &t = tasks[ti.taskIdx];

            std::string title = t["title"].asString();

            std::string status = t["status"].asString("todo");

            std::string pri = t["priority"].asString();



            // Check if this node has children in the tree
            bool hasChildren = (treeIdx + 1 < (int)g_gtdTree.size() &&
                                g_gtdTree[treeIdx + 1].depth > ti.depth);
            bool isFolded = g.foldedNodes.count(treeIdx) > 0;

            // Build tree prefix using ├─ └─ │ symbols

            std::string prefix;

            for (int a = 0; a < ti.depth; a++)

                prefix += ti.ancLast[a] ? "  " : "│ ";

            // 折叠项:三角在缩进线位置(替换 ◆/├);无折叠项:根用 ◆,子项用 ├─/└─
            if (hasChildren) prefix += isFolded ? "▸ " : "▾ ";
            else if (ti.depth > 0) prefix += ti.isLast ? "└─ " : "├─ ";
            else prefix += "◆ ";



            char line[96];

            snprintf(line, sizeof(line), "%s%s %s", prefix.c_str(), statusIcon(status), title.c_str());

            int rowY = y + i * LINE_SPACING - g.scrollFrac;

            ui_draw_text(4, rowY, line, sel);



            // Right-side info: progress + priority badge

            int pri_w = 0, pri_x = SCREEN_W;

            if (!pri.empty()) {

                pri_w = g_font.textWidth(pri.c_str()) + 4;

                pri_x = SCREEN_W - pri_w - 4;

            }



            // Progress before priority

            int pct = (int)t["progress"].asNumber(0);

            if (pct > 0) {

                const char *buf = progressIconStr(pct);

                int info_w = g_font.textWidth(buf);

                int info_x = pri_x - 6 - info_w;

                g_font.drawText(info_x, rowY, buf, false);

            }



            if (!pri.empty()) {

                u8g2_SetDrawColor(g_u8g2, 0);

                u8g2_DrawBox(g_u8g2, pri_x, rowY - g_font.ascent(), pri_w, FONT_H);

                u8g2_SetDrawColor(g_u8g2, 1);

                g_font.drawText(pri_x + 2, rowY, pri.c_str(), true);

                u8g2_SetDrawColor(g_u8g2, 0);

            }

        }

        gtdClipListTop();

        drawGtdScrollBar(y - g_font.ascent() - g.scrollFrac, gtdListBottomY());

        drawGtdFab();
        drawGtdSubFab();   // 紧挨着「+」左边：空心方框加号 = 给选中任务挂子任务

        drawGtdDirPad();   // 项目详情列表：左下角一排方向键（调顺序/层级）



        // Status bar: left=help hint | selected task info
        //
        // 左边以前是 `←→层级 ↑↓顺序` —— 那是**左下角那排方向浮动按钮**（drawGtdDirPad）
        // 说自己功能的快捷键说明，按钮画在眼前，再写一行字是重复。现在按 GTD 其余视图的
        // 口径（见任务列表那条 `?:帮助|...`）只留选中任务的 @情境/#标签/截止日期。
        char statusLine[128];
        statusLine[0] = '\0';
        if (g.sel >= 0 && g.sel < (int)g_visibleTreeIdx.size()) {
            auto &st = tasks[g_gtdTree[g_visibleTreeIdx[g.sel]].taskIdx];
            std::string parts;
            std::string sCtx = st["context"].asString();
            if (!sCtx.empty()) parts += "@" + sCtx + " ";
            auto &stt = st["tags"];
            if (stt.isArray()) {
                for (int j = 0; j < (int)stt.size(); j++) {
                    std::string tn = stt[j].asString();
                    if (!tn.empty()) parts += "#" + tn + " ";
                }
            }
            std::string sDue = st["due"].asString();
            if (!sDue.empty()) {
                size_t nd = 0;
                for (char c : sDue) if (c == '-') nd++;
                std::string dd = (nd >= 2 && sDue.length() >= 5) ? sDue.substr(sDue.length() - 5) : sDue;
                if (!dd.empty()) parts += dd + " ";
            }
            while (!parts.empty() && parts.back() == ' ') parts.pop_back();
            if (!parts.empty())
                snprintf(statusLine, sizeof(statusLine), "?:帮助|%s", parts.c_str());
        }
        if (statusLine[0] == '\0')
            snprintf(statusLine, sizeof(statusLine), "?:帮助");
        char rbuf[40]; gtdStatusRight(rbuf, sizeof(rbuf));
        ui_draw_status(statusLine, rbuf);

        drawIMEStatus();

        return;

    }



    int vis = gtdListDrawRows();

    if (vis < 1) vis = 1;



    if (g.sel < g.scroll) g.scroll = g.sel;

    if (g.sel >= g.scroll + vis) g.scroll = g.sel - vis + 1;



    auto &tasks = g.data["tasks"];

    for (int i = 0; i < vis && (g.scroll + i) < (int)g.filtered.size(); i++) {

        int ti = g.filtered[g.scroll + i];

        auto &t = tasks[ti];

        bool sel = (g.scroll + i == g.sel);

        bool msel = (!sel && isMultiSelected(g.scroll + i));

        std::string title = t["title"].asString();

        std::string status = t["status"].asString("todo");

        std::string pri = t["priority"].asString();

        std::string parent = t["parent"].asString();



        char line[160];

        snprintf(line, sizeof(line), "%s%s %s", msel ? "✓ " : "", statusIcon(status), title.c_str());



        int indent = 0;
        if (g.view == V_PROJECT && !parent.empty()) indent = 1;

        int lx = 8 + indent * 12;



        int rowY = y + i * LINE_SPACING - g.scrollFrac;

        ui_draw_text(lx, rowY, line, sel);



        // Right-side info: progress + priority badge

        int pri_w = 0, pri_x = SCREEN_W;

        if (!pri.empty()) {

            pri_w = g_font.textWidth(pri.c_str()) + 4;

            pri_x = SCREEN_W - pri_w - 4;

        }



        // Progress before priority

        int pct = (int)t["progress"].asNumber(0);

        if (pct > 0) {

            const char *buf = progressIconStr(pct);

            int info_w = g_font.textWidth(buf);

            int info_x = pri_x - 6 - info_w;

            g_font.drawText(info_x, rowY, buf, false);

        }



        if (!pri.empty()) {

            u8g2_SetDrawColor(g_u8g2, 0);

            u8g2_DrawBox(g_u8g2, pri_x, rowY - g_font.ascent(), pri_w, FONT_H);

            u8g2_SetDrawColor(g_u8g2, 1);

            g_font.drawText(pri_x + 2, rowY, pri.c_str(), true);

            u8g2_SetDrawColor(g_u8g2, 0);

        }

    }

    gtdClipListTop();

    drawGtdScrollBar(y - g_font.ascent() - g.scrollFrac, gtdListBottomY());

    drawGtdFab();



    // 筛选行：键盘弹起时贴在键盘面板上沿（列表下边界正好让出了这一行）。
    int filterLineY = (g.mode == M_FILTER && gtdVkActive())
                          ? editorVkTop() - 2
                          : STATUS_Y - LINE_SPACING + 2;

    if (!g.filterText.empty() && (!g_ime.composing() || gtdVkActive())) {

        char fb[64];

        snprintf(fb, sizeof(fb), "筛选: %s", g.filterText.c_str());

        ui_draw_text(4, filterLineY, fb, true);

    }



    // Show active context/tag filter

    if (!g.filterContext.empty() || !g.filterTags.empty()) {

        char fb[96];

        std::string ftxt;

        if (!g.filterContext.empty()) ftxt += "@" + g.filterContext;

        for (auto &ft : g.filterTags) {

            if (!ftxt.empty()) ftxt += " ";

            ftxt += "#" + ft;

        }

        snprintf(fb, sizeof(fb), "过滤: %s", ftxt.c_str());

        ui_draw_text(4, filterLineY - (g.filterText.empty() ? 0 : LINE_SPACING), fb, true);

    }



    // Status bar: left=help hint | selected task info
    char statusLine[128];
    statusLine[0] = '\0';
    if (g.sel >= 0 && g.sel < (int)g.filtered.size()) {
        auto &st = tasks[g.filtered[g.sel]];
        std::string parts;
        std::string sCtx = st["context"].asString();
        if (!sCtx.empty()) parts += "@" + sCtx + " ";
        auto &stt = st["tags"];
        if (stt.isArray()) {
            for (int j = 0; j < (int)stt.size(); j++) {
                std::string tn = stt[j].asString();
                if (!tn.empty()) parts += "#" + tn + " ";
            }
        }
        std::string sDue = st["due"].asString();
        if (!sDue.empty()) {
            size_t nd = 0;
            for (char c : sDue) if (c == '-') nd++;
            std::string dd = (nd >= 2 && sDue.length() >= 5) ? sDue.substr(sDue.length() - 5) : sDue;
            if (!dd.empty()) parts += dd + " ";
        }
        while (!parts.empty() && parts.back() == ' ') parts.pop_back();
        if (!parts.empty())
            snprintf(statusLine, sizeof(statusLine), "?:帮助|%s", parts.c_str());
    }
    if (hasMultiSel()) {
        // 多选时状态栏显示选中数量(替换任务详情信息)
        snprintf(statusLine, sizeof(statusLine), "已选%d项|?:帮助", (int)g.multiSel.size());
    }
    if (statusLine[0] == '\0')
        snprintf(statusLine, sizeof(statusLine), "?:帮助");
    char rbuf[40]; gtdStatusRight(rbuf, sizeof(rbuf));
    if (g.mode == M_FILTER && gtdVkActive()) {
        // 筛选是在列表上直接打字：键盘面板压在列表之上，候选条由键盘自带。
        editorVkDraw();
        gtdStatusBar(statusLine, rbuf);
    } else {
        ui_draw_status(statusLine, rbuf);
        drawIMEStatus();
    }

}



// ── Draw add-task input ──────────────────────────────────────────────────

static void drawAdd() {

    ui_clear();

    const char *addTitle = "添加新任务";

    if (g.mode == M_RENAME) addTitle = "重命名任务";

    else if (g.mode == M_ADD_PROJECT) addTitle = "新建项目";

    else if (g.mode == M_RENAME_PROJECT) addTitle = "重命名项目";

    else if (g.mode == M_ADD_CONTEXT) addTitle = "添加情境";

    else if (g.mode == M_ADD_TAG) addTitle = "添加标签";

    else if (g.mode == M_RENAME_CONTEXT) addTitle = "重命名情境";

    else if (g.mode == M_RENAME_TAG) addTitle = "重命名标签";

    // ui_draw_text* 的 y 是**基线**，不是行顶。原来写死 28：若字号 ascent()>28
    // （默认字体就是），字形上升部直接顶出屏幕外——标题"被截断半个字"。
    // 改成从 ascent() 让出 6px，标题在任何字号下都完整；下面几行同样跟着这个
    // 基准走，与原来的相对间距一致。
    const int topY = g_font.ascent() + 6;

    ui_draw_text_centered(topY, addTitle, false, true);

    u8g2_DrawHLine(g_u8g2, 0, topY + g_font.descent() + 4, SCREEN_W);



    // Hint for context/tag modes

    if (g.mode == M_ADD_CONTEXT || g.mode == M_RENAME_CONTEXT)

        ui_draw_text_centered(topY + FONT_H + 4, "以@开头或直接输入");

    else if (g.mode == M_ADD_TAG || g.mode == M_RENAME_TAG)

        ui_draw_text_centered(topY + FONT_H + 4, "以#开头或直接输入");



    std::string display = g.editBuf.empty() ? " " : g.editBuf;

    int ty;

    if (g.mode == M_ADD_CONTEXT || g.mode == M_ADD_TAG || g.mode == M_RENAME_CONTEXT || g.mode == M_RENAME_TAG)

        ty = topY + FONT_H * 2 + 8 + g_font.ascent();

    else

        ty = topY + g_font.descent() + 12 + g_font.ascent();

    ui_draw_text(4, ty, display.c_str());

    // 记下这一行输入框的几何：触摸长按选字 / 粘贴板反白要用（见 text_sel）。
    g.fieldX = 4;

    g.fieldBaseline = ty;

    // cursor

    int cx = g_font.textWidth(g.editBuf.substr(0, g.editCur).c_str());

    u8g2_SetDrawColor(g_u8g2, 0);

    u8g2_DrawBox(g_u8g2, 4 + cx, ty + 4, 8, 3);

    u8g2_SetDrawColor(g_u8g2, 1);



    // 虚拟键盘自带候选条；它在屏上时不能再画全屏候选面板（会盖掉键盘）。
    // 实体键盘走全屏候选面板，但这一屏的状态栏是**无条件**画的（和灵感/润色那种
    // "有面板就不画状态栏"的二选一不一样），所以面板必须用"给状态栏让位"那个版本：
    // 贴屏幕底的话候选行整个被状态栏的白底盖住（用户侧就是"打字的候选字看不见"）。
    if (gtdVkActive()) editorVkDraw();
    else if (g_ime.composing()) drawIMEUIFullscreenAboveStatusBar();
    gtdStatusBar("Enter确定 ESC取消", imeStatusLabel(g.imeActive));

    u8g2_SetDrawColor(g_u8g2, 0);

    // 触摸选区的反白 + 按钮条（会话没开就是空操作）。画在最上面：键盘在它下面。
    gtdFieldOverlay(g.editBuf);

    ui_commit();

}



static const char *HELP_LINES[] = {

    "── 列表视图 ──",

    "USER键  上选(双击切换状态)",

    "BOOT键  下选",

    "USER长按 切换标签",

    "",

    "↓     下移",

    "↑     上移",

    "j     上移任务",

    "k     下移任务",

    "h     提高层级",

    "l     降低层级",

    "z     折叠/展开",

    "Z     全部折叠/展开",

    "A     归档管理",

    "a     添加任务",

    "i     添加子任务",

    "r     重命名",

    "Enter 详情",

    "Space 切换状态",

    "Shift+↑↓ 多选",

    "d     删除(批量)",

    "Tab   切换标签",

    "/     筛选",

    "r     重命名项目",

    "n     新建项目",

    "",

    "── 任务详情 ──",

    "j/↓   下一字段",

    "k/↑   上一字段",

    "Enter 编辑字段",

    "Esc   返回",

    "优先级/状态/项目: 弹出选择",

    "截止日期: 日历选择",

    "Backspace 清除日期",

    "s     任务摘要",

    "",

    "── 日历对话框 ──",

    "←→    逐日选择",

    "↑↓    按周跳转",

    "h     上月",

    "l     下月",

    "Enter 确认",

    "Esc   取消",

    "",

    "── 添加/重命名 ──",

    "Ctrl+Space 切换输入法",

    "Enter 确认",

    "Esc   取消",

    "",

    "── 备注编辑 ──",

    "Enter 换行",

    "Tab   保存",

    "Esc   取消",

    "",

    "── 通用 ──",

    "?     显示帮助",

    "c     情境管理",

    "t     标签管理",

    "q/Esc 返回",

};

static const int HELP_LINE_COUNT = sizeof(HELP_LINES) / sizeof(HELP_LINES[0]);



static void drawSummary() {
    // Box 300x250 centered
    int boxX = (SCREEN_W - 300) / 2;
    int boxY = (SCREEN_H - 250) / 2;
    int boxW = 300, boxH = 250;
    u8g2_SetDrawColor(g_u8g2, 1);
    u8g2_DrawBox(g_u8g2, boxX, boxY, boxW, boxH);
    u8g2_SetDrawColor(g_u8g2, 0);
    u8g2_DrawFrame(g_u8g2, boxX, boxY, boxW, boxH);
    ui_draw_text_centered(boxY + FONT_H, "任务摘要", false, true);

    auto &task = g.data["tasks"][g.detailTaskIdx];
    int textX = boxX + 8;
    int y = boxY + FONT_H + 12 + 15;
    int contentW = boxW - 16;

    // Context
    std::string ctx = task["context"].asString();
    char line[128];
    snprintf(line, sizeof(line), "情境: %s", ctx.empty() ? "(无)" : ("@" + ctx).c_str());
    ui_draw_text(textX, y, line, false);
    y += LINE_SPACING;

    // Tags
    auto &tt = task["tags"];
    std::string tagStr;
    if (tt.isArray() && tt.size() > 0) {
        for (int j = 0; j < (int)tt.size(); j++) {
            if (j > 0) tagStr += " ";
            tagStr += "#" + tt[j].asString();
        }
    } else {
        tagStr = "(无)";
    }
    snprintf(line, sizeof(line), "标签: %s", tagStr.c_str());
    ui_draw_text(textX, y, line, false);
    y += LINE_SPACING;

    // Progress
    int pct = (int)task["progress"].asNumber(0);
    snprintf(line, sizeof(line), "进度: %s", progressIconStr(pct));
    ui_draw_text(textX, y, line, false);
    y += LINE_SPACING + 4;

    // Separator
    u8g2_DrawHLine(g_u8g2, boxX + 4, y - 14, boxW - 8);
    y += LINE_SPACING + 4 - 12;

    // Notes - inline word-wrap
    std::string note = task["note"].asString();
    if (!note.empty()) {
        std::vector<std::string> noteLines;
        std::string curLine;
        for (size_t k = 0; k < note.size(); k++) {
            if (note[k] == '\n') {
                noteLines.push_back(curLine);
                curLine.clear();
            } else {
                curLine += note[k];
            }
        }
        if (!curLine.empty() || noteLines.empty()) noteLines.push_back(curLine);
        std::vector<std::string> wrapped;
        for (auto &nl : noteLines) {
            int pos = 0;
            int len = (int)nl.length();
            while (pos < len) {
                int end = pos;
                int lastBreak = -1;
                while (end < len) {
                    std::string sub = nl.substr(pos, end - pos + 1);
                    if (g_font.textWidth(sub.c_str()) > contentW) break;
                    if (nl[end] == ' ') lastBreak = end + 1;
                    end++;
                }
                if (end >= len) {
                    wrapped.push_back(nl.substr(pos));
                    break;
                }
                if (lastBreak > pos) {
                    wrapped.push_back(nl.substr(pos, lastBreak - pos));
                    pos = lastBreak;
                    while (pos < len && nl[pos] == ' ') pos++;
                } else if (end > pos) {
                    wrapped.push_back(nl.substr(pos, end - pos));
                    pos = end;
                } else {
                    // Single character wider than contentW, force include it
                    wrapped.push_back(nl.substr(pos, 1));
                    pos++;
                }
            }
        }
        int textAreaH = boxY + boxH - 16 - y;
        int maxVis = textAreaH / LINE_SPACING;
        if (maxVis < 1) maxVis = 1;
        if (g.summaryScroll > (int)wrapped.size() - maxVis)
            g.summaryScroll = (int)wrapped.size() - maxVis;
        if (g.summaryScroll < 0) g.summaryScroll = 0;
        for (int i = 0; i < maxVis && (g.summaryScroll + i) < (int)wrapped.size(); i++)
            g_font.drawText(textX, y + i * LINE_SPACING, wrapped[g.summaryScroll + i].c_str(), false);
    } else {
        ui_draw_text(textX, y, "(无备注)", false);
    }

    ui_draw_status("\xe2\x86\x91\xe2\x86\x93\xe6\xbb\x9a\xe5\x8a\xa8 Esc\xe8\xbf\x94\xe5\x9b\x9e", "");
    u8g2_SetDrawColor(g_u8g2, 0);
    ui_commit();
}static void drawHelp() {

    // Draw underlying screen first

    if (g.mode == M_HELP && g.helpPrevMode == M_BROWSE) drawList();

    else drawDetail();



    int boxW = 300;

    int boxH = 250;

    int boxX = (SCREEN_W - boxW) / 2;

    int boxY = (SCREEN_H - boxH) / 2;



    // Opaque white background

    u8g2_SetDrawColor(g_u8g2, 1);

    u8g2_DrawBox(g_u8g2, boxX, boxY, boxW, boxH);

    u8g2_SetDrawColor(g_u8g2, 0);

    u8g2_DrawFrame(g_u8g2, boxX, boxY, boxW, boxH);



    // Title

    int titleY = boxY + 8 + g_font.ascent();

    g_font.drawText(boxX + (boxW - g_font.textWidth("快捷键帮助")) / 2, titleY, "快捷键帮助", false);



    // Separator

    u8g2_DrawHLine(g_u8g2, boxX + 4, titleY + g_font.descent() + 4, boxW - 8);



    // Content area

    int contentY = titleY + g_font.descent() + 10;

    int contentMaxY = boxY + boxH - 8;

    int maxVis = (contentMaxY - contentY) / LINE_SPACING;

    if (maxVis < 1) maxVis = 1;



    int maxScroll = HELP_LINE_COUNT - maxVis;

    if (maxScroll < 0) maxScroll = 0;

    if (g.helpScroll > maxScroll) g.helpScroll = maxScroll;

    if (g.helpScroll < 0) g.helpScroll = 0;



    for (int i = 0; i < maxVis && (g.helpScroll + i) < HELP_LINE_COUNT; i++) {

        int ly = contentY + i * LINE_SPACING;

        const char *line = HELP_LINES[g.helpScroll + i];

        // Section headers start with ─ (0xE2 in UTF-8)

        bool isHeader = ((unsigned char)line[0] == 0xE2);

        ui_draw_text(boxX + 12, ly + g_font.ascent(), line, false, isHeader);

    }

}



struct DetailField {

    const char *label;  // display label

    const char *key;    // actual JSON key

    char type; // 's'=string, 'p'=priority toggle, 't'=status toggle, 'n'=number, 'j'=project selector

};



static const DetailField DETAIL_FIELDS[] = {

    {"标题",     "title",    's'},

    {"优先级",   "priority", 'p'},

    {"状态",     "status",   't'},

    {"项目",     "project",  'j'},

    {"截止日期", "due",      'd'},

    {"情境",     "context",  'c'},

    {"标签",     "tags",     'g'},

    {"进度",     "progress", 'n'},

    {"备注",     "note",     'm'},

};

static const int NUM_DETAIL_FIELDS = sizeof(DETAIL_FIELDS) / sizeof(DETAIL_FIELDS[0]);



static void openPicker(int fieldIdx) {

    auto &df = DETAIL_FIELDS[fieldIdx];

    auto &task = g.data["tasks"][g.detailTaskIdx];

    g.pickerOpts.clear();

    g.pickerField = fieldIdx;



    if (df.type == 'p') {

        for (int i = 0; i < 3; i++)

            g.pickerOpts.push_back({PRIORITY_LABELS[i], PRIORITY_DISPLAY[i]});

        std::string cur = task["priority"].asString();

        if (cur.empty()) cur = "B";

        g.pickerSel = 0;

        for (int i = 0; i < 3; i++) if (PRIORITY_LABELS[i] == cur) g.pickerSel = i;

    } else if (df.type == 't') {

        for (int i = 0; i < 4; i++)

            g.pickerOpts.push_back({STATUS_LABELS[i], STATUS_DISPLAY[i]});

        std::string cur = task["status"].asString("todo");

        g.pickerSel = 0;

        for (int i = 0; i < 4; i++) if (STATUS_LABELS[i] == cur) g.pickerSel = i;

    } else if (df.type == 'j') {

        buildProjectList();

        g.pickerOpts.push_back({"", "(无)"});

        for (auto &p : g.projectList)

            g.pickerOpts.push_back({p, p});

        std::string cur = task["project"].asString();

        g.pickerSel = 0;

        for (int i = 0; i < (int)g.pickerOpts.size(); i++)

            if (g.pickerOpts[i].value == cur) { g.pickerSel = i; break; }

    } else if (df.type == 'c') {

        buildContextList();

        g.pickerOpts.push_back({"", "(无)"});

        for (auto &c : g.contextList)

            g.pickerOpts.push_back({c, "@" + c});

        std::string cur = task["context"].asString();

        g.pickerSel = 0;

        for (int i = 0; i < (int)g.pickerOpts.size(); i++)

            if (g.pickerOpts[i].value == cur) { g.pickerSel = i; break; }

    } else if (df.type == 'g') {

        buildTagList();

        for (auto &t : g.tagList)

            g.pickerOpts.push_back({t, "#" + t});

        g.pickerSel = 0;

        g.pickerToggled.clear();

        auto &tt = task["tags"];

        if (tt.isArray()) {

            for (int i = 0; i < (int)g.pickerOpts.size(); i++) {

                for (int j = 0; j < (int)tt.size(); j++) {

                    if (g.pickerOpts[i].value == tt[j].asString()) {

                        g.pickerToggled.insert(i);

                        break;

                    }

                }

            }

        }

    } else if (df.type == 'n') {

        static const int PROG_VALS[] = {0, 12, 25, 37, 50, 62, 75, 87, 100};

        for (int i = 0; i < 9; i++) {

            char label[16];

            snprintf(label, sizeof(label), " %d%%", PROG_VALS[i]);

            g.pickerOpts.push_back({std::to_string(PROG_VALS[i]),
                                    std::string(progressIconStr(PROG_VALS[i])) + label});

        }

        int cur = task["progress"].asInt(0);

        g.pickerSel = 0;

        int best = 0, bestDist = 100000;

        for (int i = 0; i < 9; i++) {

            int d = PROG_VALS[i] - cur;

            if (d < 0) d = -d;

            if (d < bestDist) { bestDist = d; best = i; }

        }

        g.pickerSel = best;

    }



    g.mode = M_PICKER;

}



static void drawPicker() {

    // Draw detail underneath first

    drawDetail();



    int n = (int)g.pickerOpts.size();

    if (n == 0) return;



    int maxVis = 6;

    int boxW = 250;

    int boxH = maxVis * LINE_SPACING + 24 + 15;

    int boxX = (SCREEN_W - boxW) / 2;

    int boxY = (SCREEN_H - boxH) / 2;



    // Scroll to keep selection visible

    int scroll = 0;

    if (n > maxVis) {

        scroll = g.pickerSel - maxVis / 2;

        if (scroll < 0) scroll = 0;

        if (scroll + maxVis > n) scroll = n - maxVis;

    }



    // Opaque white background

    u8g2_SetDrawColor(g_u8g2, 1);

    u8g2_DrawBox(g_u8g2, boxX, boxY, boxW, boxH);

    // Black border frame

    u8g2_SetDrawColor(g_u8g2, 0);

    u8g2_DrawFrame(g_u8g2, boxX, boxY, boxW, boxH);



    // Draw options

    int vis = n > maxVis ? maxVis : n;

    bool isTagPicker = (g.pickerField >= 0 && g.pickerField < NUM_DETAIL_FIELDS && DETAIL_FIELDS[g.pickerField].type == 'g');

    for (int i = 0; i < vis; i++) {

        int oi = scroll + i;

        int iy = boxY + 27 + i * LINE_SPACING;

        bool sel = (oi == g.pickerSel);

        bool toggled = isTagPicker && g.pickerToggled.count(oi);

        // For tag picker: show [✓] or [ ] prefix

        std::string display;

        if (isTagPicker) display = toggled ? "[✓]" : "[ ]";

        display += g.pickerOpts[oi].display;

        if (sel) {

            u8g2_SetDrawColor(g_u8g2, 0);

            u8g2_DrawBox(g_u8g2, boxX + 4, iy - g_font.ascent(), boxW - 8, FONT_H);

            u8g2_SetDrawColor(g_u8g2, 1);

            // invert=true：u8g2 的 draw color 对 TTF 渲染器（g_font.drawText → ttf_*）无效，
            // 它的"反色"是字形自己按 fg=15/bg=0 画白字黑底，必须显式传进去 —— 否则就是
            // 黑底黑字，选中项看不见（原样照抄 u8g2_DrawStr 的写法留下的坑）。
            g_font.drawText(boxX + 8, iy, display.c_str(), true);

            u8g2_SetDrawColor(g_u8g2, 0);

        } else {

            g_font.drawText(boxX + 8, iy, display.c_str(), false);

        }

    }



    // Scroll indicators

    if (n > maxVis) {

        if (scroll > 0) {

            char si[16]; snprintf(si, sizeof(si), "▲%d", scroll);

            g_font.drawText(boxX + boxW - g_font.textWidth(si) - 6, boxY + 27 + g_font.ascent(), si, false);

        }

        if (scroll + maxVis < n) {

            char si[16]; snprintf(si, sizeof(si), "▼%d", n - scroll - maxVis);

            g_font.drawText(boxX + boxW - g_font.textWidth(si) - 6, boxY + 27 + (vis - 1) * LINE_SPACING + g_font.ascent(), si, false);

        }

    }

}



static int daysInMonth(int year, int month) {

    static const int dim[] = {0,31,28,31,30,31,30,31,31,30,31,30,31};
    if (month < 1 || month > 12) return 30;

    int d = dim[month];

    if (month == 2 && (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0))) d = 29;

    return d;

}



// day of week: 0=Mon .. 6=Sun

static int dayOfWeek(int year, int month, int day) {

    struct tm t = {};

    t.tm_year = year - 1900;

    t.tm_mon = month - 1;

    t.tm_mday = day;

    mktime(&t);

    int w = t.tm_wday;  // 0=Sun

    return w == 0 ? 6 : w - 1;

}



static void openCalendar() {

    auto &task = g.data["tasks"][g.detailTaskIdx];

    std::string due = task["due"].asString();

    time_t now_t; time(&now_t); struct tm *now_tm = localtime(&now_t);



    if (due.length() >= 10) {

        g.calYear = atoi(due.substr(0, 4).c_str());

        g.calMonth = atoi(due.substr(5, 2).c_str());

        g.calDay = atoi(due.substr(8, 2).c_str());

    } else {

        g.calYear = now_tm->tm_year + 1900;

        g.calMonth = now_tm->tm_mon + 1;

        g.calDay = now_tm->tm_mday;

    }

    g.calSelDay = g.calDay;

    // 防御 due 字段里非法月份/日期(如 13 月 32 日)导致的越界访问。
    if (g.calMonth < 1) g.calMonth = 1;
    if (g.calMonth > 12) g.calMonth = 12;
    if (g.calDay < 1) g.calDay = 1;
    int dim = daysInMonth(g.calYear, g.calMonth);
    if (g.calDay > dim) g.calDay = dim;
    g.calSelDay = g.calDay;

    g.mode = M_CALENDAR;

}



// 日历浮层版面：竖屏也不挤。列宽随屏宽缩放、行高按字高走，浮层按内容定尺寸
// 再居中；绘制和"点哪一格"的命中判定共用这一份几何，不会对不上。
struct CalBox {
    int x, y, w, h;   // 浮层矩形
    int colW;         // 每列宽
    int hdrBase;      // 年月文字基线（绝对坐标）
    int wdBase;       // 星期文字基线
    int sepY;         // 星期与日期之间的分隔线
    int gridBase;     // 第 1 行日期文字的基线
    int rowH;         // 日期行距
};

static CalBox calBox() {
    CalBox cb{};
    // 列宽尽量摊开；上限 84 免得单列宽得不像格子，下限 34 保证数字不挤。
    cb.colW = (SCREEN_W - 40) / 7;
    if (cb.colW > 84) cb.colW = 84;
    if (cb.colW < 34) cb.colW = 34;
    cb.rowH = FONT_H + 12;
    if (cb.rowH < LINE_SPACING) cb.rowH = LINE_SPACING;
    cb.w = cb.colW * 7 + 12;
    int pad = 12;
    int ascent = g_font.ascent();
    cb.hdrBase = pad + ascent;
    cb.wdBase = cb.hdrBase + FONT_H + 8;
    cb.sepY = cb.wdBase + 4;
    cb.gridBase = cb.sepY + 4 + ascent;
    cb.h = cb.gridBase + 5 * cb.rowH + 8;
    cb.x = (SCREEN_W - cb.w) / 2;
    cb.y = (SCREEN_H - cb.h) / 2;
    // 上面算的都是"相对浮层顶"的偏移，这里统一补上绝对 y。
    cb.hdrBase += cb.y;
    cb.wdBase += cb.y;
    cb.sepY += cb.y;
    cb.gridBase += cb.y;
    return cb;
}

static void drawCalendar() {

    drawDetail();

    CalBox cb = calBox();
    int boxX = cb.x, boxY = cb.y, boxW = cb.w, boxH = cb.h;
    int colW = cb.colW;

    // Opaque white background
    u8g2_SetDrawColor(g_u8g2, 1);
    u8g2_DrawBox(g_u8g2, boxX, boxY, boxW, boxH);
    u8g2_SetDrawColor(g_u8g2, 0);
    u8g2_DrawFrame(g_u8g2, boxX, boxY, boxW, boxH);

    // Month/year header
    char hdr[32];
    snprintf(hdr, sizeof(hdr), "%d年%d月", g.calYear, g.calMonth);
    int hdrW = g_font.textWidth(hdr);
    g_font.drawText(boxX + (boxW - hdrW) / 2, cb.hdrBase, hdr, false);

    // Navigation hints: h=prev month, l=next month
    g_font.drawText(boxX + 8, cb.hdrBase, "h", false);
    g_font.drawText(boxX + boxW - 8 - g_font.textWidth("l"), cb.hdrBase, "l", false);

    // Weekday header
    const char *wdnames[] = {"一","二","三","四","五","六","日"};
    for (int i = 0; i < 7; i++) {
        int cx = boxX + 6 + i * colW + (colW - g_font.textWidth(wdnames[i])) / 2;
        g_font.drawText(cx, cb.wdBase, wdnames[i], false);
    }

    // Separator line
    u8g2_SetDrawColor(g_u8g2, 0);
    u8g2_DrawHLine(g_u8g2, boxX + 4, cb.sepY, boxW - 8);

    // Day grid
    int firstDow = dayOfWeek(g.calYear, g.calMonth, 1);
    int dim = daysInMonth(g.calYear, g.calMonth);



    time_t now_t; time(&now_t); struct tm *now_tm = localtime(&now_t);

    int todayDay = (now_tm->tm_year + 1900 == g.calYear && now_tm->tm_mon + 1 == g.calMonth)

                   ? now_tm->tm_mday : 0;



    for (int d = 1; d <= dim; d++) {

        int dow = (firstDow + d - 1) % 7;

        int row = (firstDow + d - 1) / 7;

        int cx = boxX + 6 + dow * colW;

        int cy = cb.gridBase + row * cb.rowH;



        bool sel = (d == g.calSelDay);

        bool today = (d == todayDay);



        char ds[16];

        snprintf(ds, sizeof(ds), "%d", d);

        int tw = g_font.textWidth(ds);

        int dx = cx + (colW - tw) / 2;



        if (sel) {

            u8g2_SetDrawColor(g_u8g2, 0);

            u8g2_DrawBox(g_u8g2, cx + 2, cy - g_font.ascent() - 2, colW - 4, FONT_H + 6);

            u8g2_SetDrawColor(g_u8g2, 1);

            g_font.drawText(dx, cy, ds, true);

            u8g2_SetDrawColor(g_u8g2, 0);

        } else {

            if (today) {

                // Underline today

                g_font.drawText(dx, cy, ds, false);

                u8g2_DrawHLine(g_u8g2, dx, cy + 3, tw);

            } else {

                g_font.drawText(dx, cy, ds, false);

            }

        }

    }

}



static void rebuildNoteVrows() {

    if (g.noteVrowsDirty) {

        g.noteVrows = buildVrows(g.noteLines);

        g.noteVrowsDirty = false;

    }

}

// ── 备注编辑器的触摸选区（共享件 text_sel）──────────────────────────────
// 备注的缓冲是 std::vector<std::string>（一行一条），而 text_sel 只认扁平的
// std::string：会话期间用一份"拍平镜像"（行间补一个 '\n'），每次会话动过缓冲就
// 拆回 noteLines。只发生在长按和按按钮的那几下，不碰逐字输入的热路径。
static std::string s_noteFlat;

// noteLines → 带 '\n' 的整块缓冲。
static void noteFlatten() {
    s_noteFlat.clear();
    for (size_t i = 0; i < g.noteLines.size(); i++) {
        if (i) s_noteFlat += '\n';
        s_noteFlat += g.noteLines[i];
    }
}

// flat 缓冲里第 lineIdx 行的起始偏移。
static int noteLineStart(int lineIdx) {
    int off = 0;
    for (int i = 0; i < lineIdx && i < (int)g.noteLines.size(); i++)
        off += (int)g.noteLines[i].size() + 1;
    return off;
}

// 当前 (noteRow, noteCol) 对应的 flat 偏移。
static int noteFlatCursor() { return noteLineStart(g.noteRow) + g.noteCol; }

// 拍平镜像 → noteLines，并把光标挪到 flatCur 处。
static void noteUnflatten(int flatCur) {
    g.noteLines.clear();
    int row = 0, col = 0;
    size_t pos = 0;
    for (;;) {
        const size_t nl = s_noteFlat.find('\n', pos);
        const std::string seg = (nl == std::string::npos) ? s_noteFlat.substr(pos)
                                                          : s_noteFlat.substr(pos, nl - pos);
        const int segStart = (int)pos;
        if (flatCur >= segStart && flatCur <= segStart + (int)seg.size()) {
            row = (int)g.noteLines.size();
            col = flatCur - segStart;
        }
        g.noteLines.push_back(seg);
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
    g.noteRow = row;
    g.noteCol = col;
    g.noteVrowsDirty = true;
}

// 可见 vrow → 触摸选区的行表。与 drawNoteEditor 用同一套几何（卷出去的行不进来：
// 触摸点不到，反白也只可能落在屏上）；顺带把 g.noteScroll 夹到光标可见。
static void noteEditorLineTable(std::vector<TextSelLine> &out, int &bottom) {
    rebuildNoteVrows();
    const int contentY = FONT_H + 8 + LINE_SPACING;
    const int maxY = gtdInputBottomY();
    int vis = (maxY - contentY) / LINE_SPACING;
    if (vis < 1) vis = 1;

    int cursorVrow = 0;
    for (int i = 0; i < (int)g.noteVrows.size(); i++) {
        if (g.noteVrows[i].lineIdx == g.noteRow && g.noteCol >= g.noteVrows[i].start &&
            g.noteCol <= g.noteVrows[i].end) {
            cursorVrow = i;
            break;
        }
    }
    if (cursorVrow < g.noteScroll) g.noteScroll = cursorVrow;
    if (cursorVrow >= g.noteScroll + vis) g.noteScroll = cursorVrow - vis + 1;

    out.clear();
    for (int i = 0; i < vis && (g.noteScroll + i) < (int)g.noteVrows.size(); i++) {
        const VRow &vr = g.noteVrows[g.noteScroll + i];
        const int base = noteLineStart(vr.lineIdx);
        TextSelLine ln;
        ln.start = base + vr.start;
        ln.end = base + vr.end;
        ln.x0 = 4 + vr.indentPx;
        ln.baseline = contentY + i * LINE_SPACING;
        out.push_back(ln);
    }
    bottom = gtdVkActive() ? editorVkTop() : SCREEN_H;
}



static void openNoteEditor() {

    auto &task = g.data["tasks"][g.detailTaskIdx];

    std::string note = task["note"].asString();

    g.noteLines.clear();

    size_t pos = 0;

    while (pos < note.length()) {

        size_t nl = note.find('\n', pos);

        g.noteLines.push_back((nl == std::string::npos) ? note.substr(pos) : note.substr(pos, nl - pos));

        if (nl == std::string::npos) break;

        pos = nl + 1;

    }

    if (g.noteLines.empty()) g.noteLines.push_back("");

    g.noteRow = 0;

    g.noteCol = (int)g.noteLines[0].length();

    g.noteScroll = 0;

    g.noteVrowsDirty = true;

    g.imeActive = true;

    g_ime.setActive(true);

    g.mode = M_EDIT_NOTE;

}



static void drawNoteEditor() {

    ui_clear();

    ui_draw_text(4, g_font.ascent(), "备注编辑", false, true);

    u8g2_DrawHLine(g_u8g2, 0, FONT_H + 4, SCREEN_W);



    rebuildNoteVrows();



    int contentY = FONT_H + 8 + LINE_SPACING;

    int maxY = gtdInputBottomY();

    int vis = (maxY - contentY) / LINE_SPACING;

    if (vis < 1) vis = 1;



    // Find cursor vrow

    int cursorVrow = 0;

    for (int i = 0; i < (int)g.noteVrows.size(); i++) {

        if (g.noteVrows[i].lineIdx == g.noteRow &&

            g.noteCol >= g.noteVrows[i].start &&

            g.noteCol <= g.noteVrows[i].end) {

            cursorVrow = i;

            break;

        }

    }



    // Scroll

    if (cursorVrow < g.noteScroll) g.noteScroll = cursorVrow;

    if (cursorVrow >= g.noteScroll + vis) g.noteScroll = cursorVrow - vis + 1;



    // Draw vrows

    for (int i = 0; i < vis && (g.noteScroll + i) < (int)g.noteVrows.size(); i++) {

        auto &vr = g.noteVrows[g.noteScroll + i];

        int ly = contentY + i * LINE_SPACING;

        std::string text = g.noteLines[vr.lineIdx].substr(vr.start, vr.end - vr.start);

        ui_draw_text(4 + vr.indentPx, ly, text.c_str(), false);

    }



    // Draw cursor

    {

        auto &vr = g.noteVrows[cursorVrow];

        std::string before = g.noteLines[vr.lineIdx].substr(vr.start, g.noteCol - vr.start);

        int cx = 4 + vr.indentPx + g_font.textWidth(before.c_str());

        int cy = contentY + (cursorVrow - g.noteScroll) * LINE_SPACING;

        u8g2_SetDrawColor(g_u8g2, 0);

        u8g2_DrawBox(g_u8g2, cx, cy + 4, 8, 3);

        u8g2_SetDrawColor(g_u8g2, 1);

    }

    u8g2_SetDrawColor(g_u8g2, 0);



    if (gtdVkActive()) editorVkDraw();
    else drawIMEUIWithStatusBar();

    u8g2_SetDrawColor(g_u8g2, 0);

    gtdStatusBar("Enter换行 Ctrl+S保存 ESC取消", imeStatusLabel(g.imeActive));

    // 触摸选区的反白 + 按钮条（会话没开就是空操作）。画在最上面：键盘在它下面。
    // 反白量的是拍平镜像的偏移，与 s_noteFlat 同源，所以这里直接用它。
    if (textSelActive()) {
        std::vector<TextSelLine> tl;
        int bottom = 0;
        noteEditorLineTable(tl, bottom);
        TextSelView view{tl.data(), (int)tl.size(), bottom};
        textSelDraw(s_noteFlat, view);
    }

    ui_commit();

}



static void drawDetail() {

    auto &tasks = g.data["tasks"];

    if (!tasks.isArray() || g.detailTaskIdx < 0 || g.detailTaskIdx >= (int)tasks.size()) return;

    auto &task = tasks[g.detailTaskIdx];



    ui_clear();

    ui_draw_text(4, g_font.ascent(), "任务详情", false, true);

    u8g2_DrawHLine(g_u8g2, 0, FONT_H + 4, SCREEN_W);



    int y = FONT_H + 8 + LINE_SPACING;

    // 编辑字段时输入行也要让位给输入法候选条 / 虚拟键盘
    int maxY = (g.mode == M_EDIT_FIELD) ? gtdInputBottomY() : STATUS_Y;

    int vis = (maxY - y + LINE_SPACING - 1) / LINE_SPACING;

    if (vis < 1) vis = 1;



    int fScroll = (g.detailField / vis) * vis;

    for (int i = 0; i < vis && (fScroll + i) < NUM_DETAIL_FIELDS; i++) {

        int fi = fScroll + i;

        auto &df = DETAIL_FIELDS[fi];

        bool sel = (fi == g.detailField);

        bool editing = (g.mode == M_EDIT_FIELD && fi == g.detailField);



        std::string val;

        if (editing) {

            // Show live edit buffer

            val = g.editBuf;

            if (val.empty()) val = " ";

        } else {

            switch (df.type) {

            case 's': val = task[df.key].asString(); break;

            case 'j': {

                val = task["project"].asString();

                if (val.empty()) val = "(无)";

                break;

            }

            case 'p': {

                std::string pv = task["priority"].asString();

                if (pv.empty()) pv = "B";

                for (int k = 0; k < 3; k++) if (PRIORITY_LABELS[k] == pv) { val = PRIORITY_DISPLAY[k]; break; }

                if (val.empty()) val = pv;

                break;

            }

            case 't': {

                std::string sv = task["status"].asString("todo");

                for (int k = 0; k < 4; k++) if (STATUS_LABELS[k] == sv) { val = STATUS_DISPLAY[k]; break; }

                if (val.empty()) val = sv;

                break;

            }

            case 'n': {
                int pct = task["progress"].asInt(0);
                std::string icon = progressIconStr(pct);
                val = icon + " " + std::to_string(pct) + "%";
                break;
            }

            case 'd': {

                val = task["due"].asString();

                if (val.empty()) val = "(未设)";

                break;

            }

            case 'm': {

                val = task[df.key].asString();

                if (val.empty()) val = "(空)";

                else {

                    size_t nl = val.find('\n');

                    if (nl != std::string::npos) val = val.substr(0, nl) + "…";

                }

                break;

            }

            case 'c': {

                val = task["context"].asString();

                if (val.empty()) val = "(无)";

                else val = "@" + val;

                break;

            }

            case 'g': {

                auto &tt = task["tags"];

                if (tt.isArray() && tt.size() > 0) {

                    for (int j = 0; j < (int)tt.size(); j++) {

                        if (j > 0) val += " ";

                        val += "#" + tt[j].asString();

                    }

                } else val = "(无)";

                break;

            }

            }

        }

        if (val.empty()) val = "-";



        char buf[80];

        snprintf(buf, sizeof(buf), "%s: %s", df.label, val.c_str());

        ui_draw_text(8, y + i * LINE_SPACING, buf, sel);



        // Draw cursor when editing string/number/multiline fields

        if (editing && (df.type == 's' || df.type == 'm')) {

            std::string prefix = std::string(df.label) + ": ";

            int px = 8 + g_font.textWidth(prefix.c_str());

            // 记下这一行的几何（值文本从 px 起，不是从行首 8 起）：触摸长按选字要用。
            g.fieldX = px;

            g.fieldBaseline = y + i * LINE_SPACING;

            int cx = px + g_font.textWidth(g.editBuf.substr(0, g.editCur).c_str());

            u8g2_SetDrawColor(g_u8g2, 0);

            u8g2_DrawBox(g_u8g2, cx, y + i * LINE_SPACING + 4, 8, 3);

            u8g2_SetDrawColor(g_u8g2, 1);

        }

    }



    // Show sub-task hierarchy below fields

    {

        std::string taskId = task["id"].asString();

        std::string parentId = task["parent"].asString();

        int ySub = y + vis * LINE_SPACING + 4;



        // Show parent task

        if (!parentId.empty()) {

            for (int k = 0; k < (int)tasks.size(); k++) {

                if (tasks[k]["id"].asString() == parentId) {

                    std::string pStatus = tasks[k]["status"].asString("todo");

                    char pbuf[80];

                    snprintf(pbuf, sizeof(pbuf), "↑ %s %s", statusIcon(pStatus), tasks[k]["title"].asString().c_str());

                    if (ySub + LINE_SPACING <= STATUS_Y)

                        ui_draw_text(8, ySub, pbuf, false);

                    ySub += LINE_SPACING;

                    break;

                }

            }

        }



        // Show child tasks

        int childCount = 0;

        for (int k = 0; k < (int)tasks.size(); k++) {

            if (tasks[k]["parent"].asString() == taskId) {

                childCount++;

                if (ySub + LINE_SPACING <= STATUS_Y) {

                    std::string cStatus = tasks[k]["status"].asString("todo");

                    char cbuf[80];

                    snprintf(cbuf, sizeof(cbuf), "├─ %s %s", statusIcon(cStatus), tasks[k]["title"].asString().c_str());

                    ui_draw_text(8, ySub, cbuf, false);

                }

                ySub += LINE_SPACING;

            }

        }

        // Fix last child connector

        if (childCount > 0) {

            // Redraw last child with └─

            int lastY = ySub - LINE_SPACING;

            if (lastY + LINE_SPACING <= STATUS_Y + LINE_SPACING) {

                // Find last child again

                int cc = 0;

                for (int k = 0; k < (int)tasks.size(); k++) {

                    if (tasks[k]["parent"].asString() == taskId) {

                        cc++;

                        if (cc == childCount) {

                            std::string cStatus = tasks[k]["status"].asString("todo");

                            char cbuf[80];

                            snprintf(cbuf, sizeof(cbuf), "└─ %s %s", statusIcon(cStatus), tasks[k]["title"].asString().c_str());

                            if (lastY + LINE_SPACING <= STATUS_Y)

                                ui_draw_text(8, lastY, cbuf, false);

                            break;

                        }

                    }

                }

            }

        }

    }

    {

        bool editingM = (g.mode == M_EDIT_FIELD && DETAIL_FIELDS[g.detailField].type == 'm');

        if (gtdVkActive()) editorVkDraw();     // 编辑态：键盘自带候选条，不再单画
        else drawIMEStatus();

        if (editingM)

            gtdStatusBar("Enter换行 Tab保存 ↑↓选择 ESC返回", imeStatusLabel(g.imeActive));

        else

            gtdStatusBar("Enter编辑 ↑↓选择 ESC返回 ?:帮助", "");

    }

    // 编辑字段时的触摸选区反白 + 按钮条（会话没开就是空操作）。
    // 本函数只画不推屏——ui_commit 由调用方发（见各 drawDetail(); ui_commit(); 处）。
    gtdFieldOverlay(g.editBuf);

}



// ── Screen entry ─────────────────────────────────────────────────────────

// ── 计划模式独立方向 ─────────────────────────────────────────────────────
// 与阅读模式同一个套路（screen_reader 的 applyReaderOrientation + 退出时
// board_restore_orientation）：本模式有自己的一份方向设置，进模式时套用、退出时
// 还给全局。设置项在写作模式的「显示与版式」里（计划模式方向）——**改的时候不
// 立刻转屏**，否则设置界面自己会转过去；老设置项「阅读器方向」就是这么写的。
// 空串 = 跟随全局「屏幕方向」，这时不动旋转（不写死横/竖，用户改全局方向时
// 计划模式跟着走，与改动前完全一致）。
static void applyGtdOrientation() {
    const std::string o = g_settings.getString("gtd_orientation", "");
    if (o == "portrait") board_force_portrait();
    else if (o == "landscape") board_force_landscape();
    else if (o == "auto") {
        // 自适应：拿最近一次加速度采样现判一次（判不出就保持原样，仍是"跟随全局"）。
        // 必须在这一帧排版**之前**：布局全按 SCREEN_W/H 现算，方向定了才排得对。
        // 进模式之后由 main.cpp 每轮 auto_orient_tick() 接着跟。
        bool portrait = false;
        if (auto_orient_initial(&portrait)) {
            if (portrait) board_force_portrait();
            else board_force_landscape();
        }
    }
}

// 公开版：切模式回来时只重套方向、不重跑 init（init 会把当前 tab 清成收集箱首页）。
void screen_gtd_apply_orientation() { applyGtdOrientation(); }

// ── 进入 / 离开计划模式（main.cpp 的 kScreens 生命周期钩子）───────────────────
// "本次开机只 init 一次"这条规则原来长在 main.cpp 的 scrGtd() 里（一份 `static bool
// gtdInited` + 一段解释）。搬到状态自己所在的文件：**重跑 screen_gtd_init() 会把
// g.view 清回收件箱**，而当前 tab / 项目下钻 / 光标全都活在文件级静态 g 里 —— 切模式
// 回来（或者从别的界面 Esc 回来）必须原样续上，所以只有第一次才 init。
static bool s_gtdInited = false;

void screen_gtd_enter(ScreenContext &ctx) {
    (void)ctx;
    if (!s_gtdInited) {
        screen_gtd_init();
        s_gtdInited = true;
    } else {
        // 重进只补方向：screen_gtd_exit() 离开时已经把方向还给了全局，
        // 而这次进来可能是在竖屏的写作模式里切过来的。
        screen_gtd_apply_orientation();
    }
}

void screen_gtd_leave(AppState next) {
    (void)next;
    // **故意不清 s_gtdInited**：下次进来还要接着用 g 里的 tab/下钻/光标。
    screen_gtd_exit();
}

// 退出计划模式：把方向还给全局设置（写作模式可能选了别的方向）。
void screen_gtd_exit() {
    board_restore_orientation();
}

void screen_gtd_init() {

    mkdir(DATA_DIR, 0755);

    // 方向要在第一帧排版之前定下来（SCREEN_W/H 是运行时取旋转算出来的）。
    applyGtdOrientation();

    // 编辑态虚拟键盘：未连蓝牙键盘就默认展开（进编辑界面时才画出来）。
    editorVkInit();

    g.mode = M_BROWSE;

    g.view = 0;

    g.sel = 0;

    g.scroll = 0;

    g.detailTaskIdx = -1;

    g.editBuf.clear();

    g.editCur = 0;

    g.imeActive = false;

    g.pendingParent.clear();

    g.filterText.clear();

    g.projectDrillIdx = -1;

    g.insertAfter = -1;

    g.confirmIdx = -1;

    g.confirmIdxs.clear();

    clearMultiSel();

    g_ime.setActive(false);

    loadData();

    autoArchive();

}



// ── 长按列表项：编辑菜单 ────────────────────────────────────────────────
// 触屏没有键盘的 r/d/h/l/i 快捷键，长按某一项就弹一个小浮层。
// 菜单只负责"选中项 + 命中"：动作要么交给既有入口（gtdBeginRename /
// gtdBeginDelete），要么**直接把键码换成对应的普通键**往下走（挂子任务='i'、
// 提升层级='h'、降低层级='l'），与键盘完全同一条实现，不会跑偏。
//
// 菜单项按上下文变：项目列表层（还没进项目）只有重命名项目/删除项目；
// **项目详情层**（下钻后的任务树）多出挂子任务与升降层级 —— 那三件事在键盘上是
// i/h/l，触屏没有键，只能靠这个浮层。
enum GtdMenuAct { GMA_RENAME, GMA_SUBTASK, GMA_PROMOTE, GMA_DEMOTE, GMA_DELETE, GMA_CANCEL };

static const GtdMenuAct kMenuProjList[] = { GMA_RENAME, GMA_DELETE, GMA_CANCEL };
static const GtdMenuAct kMenuTask[]     = { GMA_RENAME, GMA_DELETE, GMA_CANCEL };
static const GtdMenuAct kMenuProject[]  = { GMA_RENAME, GMA_SUBTASK, GMA_PROMOTE, GMA_DEMOTE,
                                            GMA_DELETE, GMA_CANCEL };

static const GtdMenuAct *gtdItemMenuActs(int *n) {
    if (isInProjectList()) { *n = 3; return kMenuProjList; }
    if (g.view == V_PROJECT && g.projectDrillIdx >= 0) { *n = 6; return kMenuProject; }
    *n = 3;
    return kMenuTask;
}

static const char *gtdItemMenuLabel(GtdMenuAct a) {
    bool proj = isInProjectList();
    switch (a) {
        case GMA_RENAME:  return proj ? "重命名项目" : "重命名";
        case GMA_SUBTASK: return "添加子任务";
        case GMA_PROMOTE: return "提升层级";
        case GMA_DEMOTE:  return "降低层级";
        case GMA_DELETE:  return proj ? "删除项目" : "删除";
        default:          return "取消";
    }
}

static void itemMenuBoxRect(int *bx, int *by, int *bw, int *bh) {
    int n = 0;
    const GtdMenuAct *acts = gtdItemMenuActs(&n);
    int w = 0;
    for (int i = 0; i < n; i++) {
        int tw = g_font.textWidth(gtdItemMenuLabel(acts[i])) + 2 * FONT_H;
        if (tw > w) w = tw;
    }
    if (w < 160) w = 160;
    if (w > SCREEN_W - 32) w = SCREEN_W - 32;
    *bw = w;
    *bh = n * LINE_SPACING + 16;
    *bx = (SCREEN_W - w) / 2;
    *by = (SCREEN_H - *bh) / 2;
}

// 菜单里每一项文字的基线（相对浮层顶，供绘制与命中共用）。
static int itemMenuRowY(int by, int i) { return by + 8 + g_font.ascent() + i * LINE_SPACING; }

static void drawItemMenu() {
    drawList();   // 底下的列表照画，浮层盖在上面

    int bx, by, bw, bh;
    itemMenuBoxRect(&bx, &by, &bw, &bh);

    u8g2_SetDrawColor(g_u8g2, 1);
    u8g2_DrawBox(g_u8g2, bx, by, bw, bh);
    u8g2_SetDrawColor(g_u8g2, 0);
    u8g2_DrawFrame(g_u8g2, bx, by, bw, bh);

    int mn = 0;
    const GtdMenuAct *acts = gtdItemMenuActs(&mn);
    for (int i = 0; i < mn; i++) {
        const char *lb = gtdItemMenuLabel(acts[i]);
        int y = itemMenuRowY(by, i);
        int tx = bx + (bw - g_font.textWidth(lb)) / 2;
        if (i == g.itemMenuSel) {
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

// 键盘 r 与长按菜单「重命名」共用的入口：任务列表 / 项目列表两种目标。
// 返回 true 表示已进入重命名态（调用方需要随后把界面画出来）。
static bool gtdBeginRename() {
    auto &tasks = g.data["tasks"];
    if (isInProjectList()) {
        if (g.sel < 0 || g.sel >= (int)g.projectList.size()) return false;
        g.renameTargetProject = g.projectList[g.sel];
        g.editBuf = g.projectList[g.sel];
        g.editCur = (int)g.editBuf.length();
        g.imeActive = true;
        g_ime.setActive(true);
        g.mode = M_RENAME_PROJECT;
        return true;
    }
    if (g.sel < 0 || g.sel >= (int)g.filtered.size()) return false;
    g.detailTaskIdx = g.filtered[g.sel];
    g.editBuf = tasks[g.detailTaskIdx]["title"].asString();
    g.editCur = (int)g.editBuf.length();
    g.imeActive = true;
    g_ime.setActive(true);
    g.mode = M_RENAME;
    return true;
}

// 键盘 d 与长按菜单「删除」共用的入口。任务走确认框；项目非空不让删（同键盘）。
static bool gtdBeginDelete(ScreenContext &ctx) {
    auto &tasks = g.data["tasks"];
    if (isInProjectList()) {
        if (g.sel < 0 || g.sel >= (int)g.projectList.size()) return false;
        std::string projName = g.projectList[g.sel];
        int taskCount = 0;
        for (int i = 0; i < (int)tasks.size(); i++) {
            if (tasks[i]["project"].asString() == projName) taskCount++;
        }
        if (taskCount > 0) {
            ctx.statusMessage = "项目非空，无法删除";
            return false;
        }
        auto &projs = g.data["projects"];
        for (int i = (int)projs.size() - 1; i >= 0; i--) {
            if (projs[i].asString() == projName)
                projs.elements.erase(projs.elements.begin() + i);
        }
        saveData();
        buildProjectList();
        if (g.sel >= (int)g.projectList.size()) g.sel = (int)g.projectList.size() - 1;
        if (g.sel < 0) g.sel = 0;
        // 浮动提示（不清屏、不阻塞），与写作模式里删日记同一套（见 ui_helpers.h）。
        ui_toast_show("已删除项目");
        return false;   // 没有确认框要弹，调用方回到列表即可
    }
    if (g.sel < 0 || g.sel >= (int)g.filtered.size()) return false;
    int idx = g.filtered[g.sel];
    g.confirmIdx = idx;
    g.confirmMsg = std::string("删除任务「") + tasks[idx]["title"].asString() + "」?";
    g.mode = M_CONFIRM;
    return true;
}

// ── Main handle ──────────────────────────────────────────────────────────

// 点按坐标 → 动作。M_ARCHIVE / 情境 / 标签管理等少数列表仍只认键盘
// （点按退化成 Enter，至少不会误触）；主列表、详情、选择器三个高频界面全覆盖。
// 放在这里是因为要用 DETAIL_FIELDS / NUM_DETAIL_FIELDS（定义在详情面板那一节）。
static int gtdKeyFromTap(int x, int y) {
    // 长按菜单是浮层，优先按它的版面判定：点菜单项 = 回车，点浮层外 = 关掉菜单。
    if (g.mode == M_ITEM_MENU) {
        int bx, by, bw, bh;
        itemMenuBoxRect(&bx, &by, &bw, &bh);
        if (x < bx || x >= bx + bw || y < by || y >= by + bh) {
            g.mode = M_BROWSE;
            return 0;
        }
        int mn2 = 0;
        gtdItemMenuActs(&mn2);
        int r = gtdRowAtY(itemMenuRowY(by, 0), y, mn2);
        if (r < 0) return 0;
        g.itemMenuSel = r;
        return 0x0A;
    }

    // 日历浮层：点 h/l 换月、点日期直接选中（再点一次确认）。浮层以外一律吃掉，
    // 否则点空白会被当成 Enter 立刻把当前选中日期定死，很别扭。
    if (g.mode == M_CALENDAR) {
        CalBox cb = calBox();
        if (x < cb.x || x >= cb.x + cb.w || y < cb.y || y >= cb.y + cb.h) return -1;
        if (y < cb.wdBase - g_font.ascent())            // 年月那一行：左半上月、右半下月
            return (x < cb.x + cb.w / 2) ? 'h' : 'l';
        int gridTop = cb.sepY + 4;
        if (y < gridTop) return 0;                      // 星期行：吃掉
        int row = (y - gridTop) / cb.rowH;
        int col = (x - (cb.x + 6)) / cb.colW;
        if (row < 0 || row > 5 || col < 0 || col > 6) return 0;
        int firstDow = dayOfWeek(g.calYear, g.calMonth, 1);
        int dim = daysInMonth(g.calYear, g.calMonth);
        int day = row * 7 + col - firstDow + 1;
        if (day < 1 || day > dim) return 0;
        if (day == g.calSelDay) return 0x0A;            // 再点一次 = 确认
        g.calSelDay = day;                              // 就地选中，重绘即反馈
        return 0;
    }

    // 选择器是浮层，优先按它的版面判定（矩形外的不认领）。
    if (g.mode == M_PICKER) {
        int n = (int)g.pickerOpts.size();
        if (n == 0) return -1;
        const int maxVis = 6, boxW = 250;
        int boxH = maxVis * LINE_SPACING + 39;
        int boxX = (SCREEN_W - boxW) / 2, boxY = (SCREEN_H - boxH) / 2;
        if (x < boxX || x >= boxX + boxW) return -1;
        int vis = n > maxVis ? maxVis : n;
        int scroll = 0;
        if (n > maxVis) {
            scroll = g.pickerSel - maxVis / 2;
            if (scroll < 0) scroll = 0;
            if (scroll + maxVis > n) scroll = n - maxVis;
        }
        int r = gtdRowAtY(boxY + 27, y, vis);
        if (r < 0) return -1;
        g.pickerSel = scroll + r;
        return 0x0A;   // 交给选择器原有的确认逻辑
    }

    // 详情页：点哪一行就选中哪一栏，再走原有的 Enter（按类型开选择器/日历/编辑）。
    if (g.mode == M_DETAIL) {
        if (y < FONT_H + 8) return -1;
        int y0 = FONT_H + 8 + LINE_SPACING;
        int vis = (STATUS_Y - y0 + LINE_SPACING - 1) / LINE_SPACING;
        if (vis < 1) vis = 1;
        int fScroll = (g.detailField / vis) * vis;
        int r = gtdRowAtY(y0, y, vis);
        if (r < 0 || fScroll + r >= NUM_DETAIL_FIELDS) return -1;
        g.detailField = fScroll + r;
        return 0x0A;
    }

    if (g.mode != M_BROWSE) return -1;

    // 悬浮「+」：项目列表层（还没进任何项目）时 = 新建项目，进了项目里 = 新建任务。
    // 都只是喂一个普通键码，复用既有的 'n' / 'a' 处理，不另写一套新建逻辑。
    // 「+子任务」：只在项目详情里存在，喂 'i'（选中任务下挂子任务）。
    if (gtdSubFabHit(x, y)) return 'i';
    if (gtdFabHit(x, y)) return isInProjectList() ? 'n' : 'a';

    // 方向浮动按钮（项目详情列表）：喂键盘上对应的 h/l/j/k，走下面同一条重排逻辑。
    {
        int di = gtdDirPadHit(x, y);
        if (di >= 0) return kDirPadKey[di];
    }

    // 顶部标签栏：点哪一格切到哪个视图。命中带从屏幕上沿一路给到标签栏下方一点，
    // 不按 56px 的字形框判——e-ink 上手指落点很糙，整条横带都算（和阅读模式 tabHit 一致）。
    if (y < TAB_BAND_BOTTOM + 6) {
        gtdSetView(gtdTabAtX(x));
        return 0;
    }
    if (y < 0 || y >= STATUS_BAR_Y) return -1;

    // 列表区：点中哪一行就选中它，然后交给 Enter 做「进入项目 / 打开详情」。
    // 减去滚动的小数偏移，命中带才和画出来的行对齐（所见即所点）。
    int rows = gtdListRows();
    int r = gtdRowAtY(gtdFirstRowY() - g.scrollFrac, y, rows);
    if (r < 0) return 0;    // 点在列表空白处：吃掉，免得突然打开当前选中项
    g.sel = g.scroll + r;
    return 0x0A;
}

// ── 共享的单行输入框触摸编辑（text_sel）─────────────────────────────────
// 计划模式的输入框都是"一行 std::string + 字节光标"，与灵感 / 设置 / flomo 同一种
// 形状，所以共用 text_sel：同一份 clipboard、同一个按钮条控件——和虚拟键盘一样，
// 是三模式共享的底层件。编辑器正文另有实现（折行 / markdown / 竖排），但两边
// 落到同一份粘贴板上。
//
// 能走这条路的模式（都只有一个扁平字段，字段几何由绘制时记下的 g.field* 给出）：
// 新建 / 重命名任务、新建 / 重命名项目、新建 / 重命名情境 / 标签、详情里的编辑字段。
// 筛选行（M_FILTER）不给：它按设计只追加、没有光标，粘贴板在那儿没什么用。

// 当前模式要编辑哪个缓冲 + 它的屏幕几何。返回 false = 这个模式没有扁平输入框。
// 这些字段都是**单独一行**画的（整块缓冲就在一行上），行表因此只有一行；
// 多行宿主（设置文本框 / 润色提示词 / 备注编辑器）各自给各自的行表。
static bool gtdFlatField(std::string **buf, int **cur, TextSelLine *ln, TextSelView *view) {
    if (g.fieldX <= 0) return false;   // 本帧没画出可编辑字段（绘制时会记）
    switch (g.mode) {
    case M_ADD: case M_RENAME:
    case M_ADD_PROJECT: case M_RENAME_PROJECT:
    case M_ADD_CONTEXT: case M_RENAME_CONTEXT:
    case M_ADD_TAG: case M_RENAME_TAG:
    case M_EDIT_FIELD:
        break;
    default:
        return false;
    }
    *buf = &g.editBuf;
    *cur = &g.editCur;
    ln->start = 0;
    ln->end = (int)g.editBuf.size();
    ln->x0 = g.fieldX;
    ln->baseline = g.fieldBaseline;
    view->lines = ln;
    view->count = 1;
    view->bottom = gtdVkActive() ? editorVkTop() : SCREEN_H;
    return true;
}

// ── 同一个"当前编辑缓冲"的 ImeField 形态 ────────────────────────────────
// 落串/退格/光标左右移全部走 ui/ime_field.h 那一份算术（UTF-8 边界只有一处实现）。
// 模式与缓冲的对应关系跟 gtdFlatField 是一回事，只是那边还要屏幕几何（触摸选字用）。
// 每个都是**当场构造**的轻量视图（两个指针），不持有所有权。
static ImeField gtdEditField()  { return ImeField{&g.editBuf, &g.editCur}; }
static ImeField gtdNoteField()  { return ImeField{&g.noteLines[g.noteRow], &g.noteCol}; }
static ImeField gtdFilterField() { return ImeField{&g.filterText, /*cursor=*/nullptr}; }

// 选区会话期间"本模式重绘一次"。按钮条贴着虚拟键盘上沿，所以这里就是普通重绘。
static void gtdRedrawFieldMode() {
    switch (g.mode) {
    case M_ADD: case M_RENAME:
    case M_ADD_PROJECT: case M_RENAME_PROJECT:
    case M_ADD_CONTEXT: case M_RENAME_CONTEXT:
    case M_ADD_TAG: case M_RENAME_TAG:
        drawAdd();
        break;
    default:
        drawDetail();
        ui_commit();   // drawDetail 只画不推
        break;
    }
}

static void gtdFieldOverlay(const std::string &buf) {
    if (!textSelActive()) return;
    TextSelLine ln;
    ln.start = 0;
    ln.end = (int)buf.size();
    ln.x0 = g.fieldX;
    ln.baseline = g.fieldBaseline;
    TextSelView view{&ln, 1, gtdVkActive() ? editorVkTop() : SCREEN_H};
    textSelDraw(buf, view);
}

// P3c：原来 2415 行的 screen_gtd_handle 按 g.mode 拆成下列函数（逐个原样搬出，
// 逻辑未改）。这里先给一组前置声明 —— gtdHandleItemMenu 的 handOff 分支要落到
// 列表主体 gtdHandleBrowse，而后者在文件末尾定义。
static AppState gtdHandleBrowse(int key, ScreenContext &ctx);

// 从 screen_gtd_handle 里原样搬出来的一个 g.mode 分支（见那边的分发表）。
static AppState gtdHandleAdd(int key) {
    auto &tasks = g.data["tasks"];


        if (g.imeActive && key != 0) {

            std::string imeOut;

            if (imeFieldKeyText(g_ime, key, false, imeOut)) {

                if (!imeOut.empty()) imeFieldInsert(gtdEditField(), imeOut);

                drawAdd();

                return APP_GTD;

            }

        }

        if (key == KEY_IME_TOGGLE) {

            g.imeActive = !g.imeActive;

            g_ime.setActive(g.imeActive);

            drawAdd();

            return APP_GTD;

        }

        if (key == 0x1B) {

            g.mode = M_BROWSE;

            g.imeActive = false; g_ime.setActive(false);

            g.insertAfter = -1;

            g.pendingParent.clear();

        } else if (key == 0x0A || key == 0x0D) {

            if (!g.editBuf.empty()) {

                JsonValue t;

                t.set("id", makeId());

                t.set("title", g.editBuf);

                t.set("priority", "B");

                t.set("status", "todo");

                t.set("due", "");

                t.set("progress", 0);

                t.set("note", "");

                t.set("context", "");

                t.set("tags", JsonValue::array());

                t.set("project", g.pendingProject.empty() ? std::string("") : g.pendingProject);

                t.set("parent", g.pendingParent);

                t.set("created", [](){

                    time_t n; time(&n); struct tm *tm = localtime(&n);

                    char b[16]; strftime(b, sizeof(b), "%Y-%m-%d", tm); return std::string(b);

                }());

                t.set("completed", "");

                if (g.insertAfter >= 0 && g.insertAfter < (int)tasks.size())

                    tasks.elements.insert(tasks.elements.begin() + g.insertAfter + 1, t);

                else

                    tasks.pushBack(t);

                saveData();

                rebuildFilter();

                g.pendingParent.clear();

                g.pendingProject.clear();

                g.insertAfter = -1;

            }

            g.mode = M_BROWSE;

            g.imeActive = false; g_ime.setActive(false);

        } else if (key == 0x7F || key == 0x08) {

            imeFieldBackspace(gtdEditField());

        } else if (key == KEY_LEFT) {

            imeFieldMoveLeft(gtdEditField());

        } else if (key == KEY_RIGHT) {

            imeFieldMoveRight(gtdEditField());

        } else if (key >= 0x20 && key <= 0x7E) {

            imeFieldInsert(gtdEditField(), std::string(1, (char)key));

        }

        drawAdd();

        return APP_GTD;

}

// 从 screen_gtd_handle 里原样搬出来的一个 g.mode 分支（见那边的分发表）。
static AppState gtdHandleRename(int key) {
    auto &tasks = g.data["tasks"];


        if (g.imeActive && key != 0) {

            std::string imeOut;

            if (imeFieldKeyText(g_ime, key, false, imeOut)) {

                if (!imeOut.empty()) imeFieldInsert(gtdEditField(), imeOut);

                drawAdd();

                return APP_GTD;

            }

        }

        if (key == KEY_IME_TOGGLE) {

            g.imeActive = !g.imeActive;

            g_ime.setActive(g.imeActive);

            drawAdd();

            return APP_GTD;

        }

        if (key == 0x1B) {

            g.mode = M_BROWSE;

            g.imeActive = false; g_ime.setActive(false);

        } else if (key == 0x0A || key == 0x0D) {

            if (!g.editBuf.empty() && g.detailTaskIdx >= 0 && g.detailTaskIdx < (int)tasks.size()) {

                tasks[g.detailTaskIdx].set("title", g.editBuf);

                saveData();

                rebuildFilter();

            }

            g.mode = M_BROWSE;

            g.imeActive = false; g_ime.setActive(false);

        } else if (key == 0x7F || key == 0x08) {

            imeFieldBackspace(gtdEditField());

        } else if (key == KEY_LEFT) {

            imeFieldMoveLeft(gtdEditField());

        } else if (key == KEY_RIGHT) {

            imeFieldMoveRight(gtdEditField());

        } else if (key >= 0x20 && key <= 0x7E) {

            imeFieldInsert(gtdEditField(), std::string(1, (char)key));

        }

        drawAdd();

        return APP_GTD;

}

// 从 screen_gtd_handle 里原样搬出来的一个 g.mode 分支（见那边的分发表）。
static AppState gtdHandleItemMenu(int key, ScreenContext &ctx) {
        int mn = 0;
        const GtdMenuAct *acts = gtdItemMenuActs(&mn);
        bool handOff = false;   // 退出浮层并把动作交给下面的键盘逻辑
        if (key == KEY_UP || key == 'k') {
            if (g.itemMenuSel > 0) g.itemMenuSel--;
        } else if (key == KEY_DOWN || key == 'j') {
            if (g.itemMenuSel < mn - 1) g.itemMenuSel++;
        } else if (key == 0x0A || key == 0x0D) {
            int sel = g.itemMenuSel;
            GtdMenuAct a = (sel >= 0 && sel < mn) ? acts[sel] : GMA_CANCEL;
            g.mode = M_BROWSE;      // 先退出浮层；下面各分支会切到目标态
            if (a == GMA_RENAME) {
                if (gtdBeginRename()) {
                    drawAdd();      // 重命名复用添加任务那个输入浮层
                    ui_commit();
                    return APP_GTD;
                }
            } else if (a == GMA_DELETE) {
                if (gtdBeginDelete(ctx)) return APP_GTD;   // 下一帧由 M_CONFIRM 画确认框
            } else if (a == GMA_SUBTASK || a == GMA_PROMOTE || a == GMA_DEMOTE) {
                key = (a == GMA_SUBTASK) ? 'i' : (a == GMA_PROMOTE) ? 'h' : 'l';
                handOff = true;
            } else {
                // 取消：回到列表
                drawList();
                ui_commit();
                return APP_GTD;
            }
        } else if (key == 0x1B || key == 'q' || key == 'Q') {
            g.mode = M_BROWSE;
        }

        if (!handOff) {
            ui_render_begin_overlay();   // 叠在上一帧上（见 ui_render.h）
            drawItemMenu();
            return APP_GTD;
        }
        // handOff：浮层已关，key 已换成 'i'/'h'/'l'，往下走同一条键盘逻辑
    // handOff：key 已换成 'i'/'h'/'l'，落到列表那条路上（与原来"掉出本块"等价）。
    return gtdHandleBrowse(key, ctx);

}

// 从 screen_gtd_handle 里原样搬出来的一个 g.mode 分支（见那边的分发表）。
static AppState gtdHandleConfirm(int key, ScreenContext &ctx) {
    auto &tasks = g.data["tasks"];


        ui_clear();

        ui_draw_text_centered(SCREEN_H / 2, g.confirmMsg.c_str(), false, true);

        ui_draw_status("Enter确认 ESC取消", "");

        ui_commit();

        if (key == 0x0A || key == 0x0D) {

            if (!g.confirmIdxs.empty()) {

                // 批量删除: 升序后从高下标往低擦除,避免下标漂移
                std::sort(g.confirmIdxs.begin(), g.confirmIdxs.end());
                for (int n = (int)g.confirmIdxs.size() - 1; n >= 0; n--) {
                    int idx = g.confirmIdxs[n];
                    if (idx >= 0 && idx < (int)tasks.size())
                        tasks.elements.erase(tasks.elements.begin() + idx);
                }
                saveData();
                rebuildFilter();
                clearMultiSel();
                if (g.sel >= (int)g.filtered.size()) g.sel = (int)g.filtered.size() - 1;
                if (g.sel < 0) g.sel = 0;
                ui_toast_show("已删除");
                g.confirmIdxs.clear();

            } else if (g.confirmIdx >= 0 && g.confirmIdx < (int)tasks.size()) {

                tasks.elements.erase(tasks.elements.begin() + g.confirmIdx);

                saveData();

                rebuildFilter();

                ui_toast_show("已删除");

            }

            g.confirmIdx = -1;

            g.mode = M_BROWSE;

        } else if (key == 0x1B) {

            // 取消: 清确认列表但保留多选, 允许反悔后继续操作
            g.confirmIdxs.clear();

            g.confirmIdx = -1;

            g.mode = M_BROWSE;

        }

        return APP_GTD;

}

// 从 screen_gtd_handle 里原样搬出来的一个 g.mode 分支（见那边的分发表）。
static AppState gtdHandleTaxNameEdit(int key) {
    auto &tasks = g.data["tasks"];


        if (g.imeActive && key != 0) {

            std::string imeOut;

            if (imeFieldKeyText(g_ime, key, false, imeOut)) {

                if (!imeOut.empty()) imeFieldInsert(gtdEditField(), imeOut);

                drawAdd(); return APP_GTD;

            }

        }

        if (key == KEY_IME_TOGGLE) { g.imeActive = !g.imeActive; g_ime.setActive(g.imeActive); drawAdd(); return APP_GTD; }

        if (key == 0x1B) {

            g.mode = M_BROWSE; g.imeActive = false; g_ime.setActive(false);

        } else if (key == 0x0A || key == 0x0D) {

            if (!g.editBuf.empty()) {

                auto &projs = g.data["projects"];

                if (g.mode == M_ADD_PROJECT) {

                    // Add new project name to stored list

                    projs.pushBack(g.editBuf);

                    saveData();

                    buildProjectList();

                } else if (g.mode == M_RENAME_PROJECT) {

                    // Rename: update all tasks + stored projects list

                    for (int i = 0; i < (int)tasks.size(); i++) {

                        if (tasks[i]["project"].asString() == g.renameTargetProject)

                            tasks[i].set("project", g.editBuf);

                    }

                    for (int i = 0; i < (int)projs.size(); i++) {

                        if (projs[i].asString() == g.renameTargetProject) {

                            projs.elements[i] = g.editBuf;

                        }

                    }

                    saveData();

                    buildProjectList();

                } else if (g.mode == M_RENAME_CONTEXT) {

                    // Rename context: update all tasks + stored list

                    auto &ctxArr = g.data["contexts"];

                    for (int i = 0; i < (int)tasks.size(); i++) {

                        if (tasks[i]["context"].asString() == g.renameTargetContext)

                            tasks[i].set("context", g.editBuf);

                    }

                    for (int i = 0; i < (int)ctxArr.size(); i++) {

                        if (ctxArr[i].asString() == g.renameTargetContext)

                            ctxArr.elements[i] = g.editBuf;

                    }

                    if (g.filterContext == g.renameTargetContext)

                        g.filterContext = g.editBuf;

                    saveData();

                    buildContextList();

                } else if (g.mode == M_RENAME_TAG) {

                    // Rename tag: update all tasks + stored list

                    auto &tagArr = g.data["tags"];

                    for (int i = 0; i < (int)tasks.size(); i++) {

                        auto &tt = tasks[i]["tags"];

                        if (tt.isArray()) {

                            for (int j = 0; j < (int)tt.size(); j++) {

                                if (tt[j].asString() == g.renameTargetTag)

                                    tt.elements[j] = g.editBuf;

                            }

                        }

                    }

                    for (int i = 0; i < (int)tagArr.size(); i++) {

                        if (tagArr[i].asString() == g.renameTargetTag)

                            tagArr.elements[i] = g.editBuf;

                    }

                    for (int i = 0; i < (int)g.filterTags.size(); i++) {

                        if (g.filterTags[i] == g.renameTargetTag)

                            g.filterTags[i] = g.editBuf;

                    }

                    saveData();

                    buildTagList();

                }

            }

            g.mode = M_BROWSE; g.imeActive = false; g_ime.setActive(false);

        } else if (key == 0x7F || key == 0x08) {

            imeFieldBackspace(gtdEditField());

        } else if (key >= 0x20 && key <= 0x7E) { imeFieldInsert(gtdEditField(), std::string(1, (char)key)); }

        drawAdd();

        return APP_GTD;

}

// 从 screen_gtd_handle 里原样搬出来的一个 g.mode 分支（见那边的分发表）。
static AppState gtdHandleDetailViews(int key) {
    auto &tasks = g.data["tasks"];


        if (g.mode == M_CALENDAR) {

            if (key == 0x1B) {

                g.mode = M_DETAIL;

            } else if (key == KEY_LEFT) {

                // Previous day

                if (g.calSelDay > 1) g.calSelDay--;

            } else if (key == KEY_RIGHT) {

                // Next day

                int dim = daysInMonth(g.calYear, g.calMonth);

                if (g.calSelDay < dim) g.calSelDay++;

            } else if (key == KEY_UP || key == 'k') {

                g.calSelDay -= 7;

                if (g.calSelDay < 1) g.calSelDay = 1;

            } else if (key == KEY_DOWN || key == 'j') {

                g.calSelDay += 7;

                int dim = daysInMonth(g.calYear, g.calMonth);

                if (g.calSelDay > dim) g.calSelDay = dim;

            } else if (key == 'h' || key == 'H') {

                // Previous month

                g.calMonth--;

                if (g.calMonth < 1) { g.calMonth = 12; g.calYear--; }

                int dim = daysInMonth(g.calYear, g.calMonth);

                if (g.calSelDay > dim) g.calSelDay = dim;

            } else if (key == 'l' || key == 'L') {

                // Next month

                g.calMonth++;

                if (g.calMonth > 12) { g.calMonth = 1; g.calYear++; }

                int dim = daysInMonth(g.calYear, g.calMonth);

                if (g.calSelDay > dim) g.calSelDay = dim;

            } else if (key == 0x0A || key == 0x0D) {

                // Confirm date

                auto &task = tasks[g.detailTaskIdx];

                char ds[16];

                snprintf(ds, sizeof(ds), "%04d-%02d-%02d", g.calYear, g.calMonth, g.calSelDay);

                task.set("due", ds);

                saveData();

                rebuildFilter();

                g.mode = M_DETAIL;

            } else if (key == 0x7F || key == 0x08) {

                // Clear date

                auto &task = tasks[g.detailTaskIdx];

                task.set("due", "");

                saveData();

                rebuildFilter();

                g.mode = M_DETAIL;

            }

            drawCalendar();

            ui_commit();

            return APP_GTD;

        }



        if (g.mode == M_PICKER) {

            auto &df = DETAIL_FIELDS[g.pickerField];

            // 'g'（标签）选择器在一条标签都没有时是空的：drawPicker 会直接不画，
            // 但按键侧若继续走 pickerOpts[g.pickerSel] 就是空 vector 越界读 → 崩。
            // 空选择器直接退回详情页（它本来就不可见，用户看到的就是"没反应"）。
            if (g.pickerOpts.empty()) {

                g.mode = M_DETAIL;

                ui_render_begin_overlay();

                drawDetail();

                ui_commit();

                return APP_GTD;

            }

            if (key == 0x1B) {

                g.mode = M_DETAIL;

            } else if (key == KEY_UP || key == 'k') {

                if (g.pickerSel > 0) g.pickerSel--;

            } else if (key == KEY_DOWN || key == 'j') {

                if (g.pickerSel < (int)g.pickerOpts.size() - 1) g.pickerSel++;

            } else if (key == ' ' && df.type == 'g') {

                // Toggle multi-select for tags

                if (g.pickerToggled.count(g.pickerSel))

                    g.pickerToggled.erase(g.pickerSel);

                else

                    g.pickerToggled.insert(g.pickerSel);

            } else if (key == 0x0A || key == 0x0D) {

                // Apply selection

                auto &task = tasks[g.detailTaskIdx];

                std::string val = g.pickerOpts[g.pickerSel].value;

                if (df.type == 'p') task.set("priority", val);

                else if (df.type == 't') task.set("status", val);

                else if (df.type == 'j') task.set("project", val);

                else if (df.type == 'c') task.set("context", val);

                else if (df.type == 'n') task.set("progress", std::stoi(val));

                else if (df.type == 'g') {

                    JsonValue tags(JsonValue::array());

                    for (int i = 0; i < (int)g.pickerOpts.size(); i++) {

                        if (g.pickerToggled.count(i))

                            tags.pushBack(g.pickerOpts[i].value);

                    }

                    task.set("tags", tags);

                }

                saveData();

                if (g.view == V_PROJECT) buildProjectList();

                rebuildFilter();

                g.mode = M_DETAIL;

            }

            ui_render_begin_overlay();   // 叠在上一帧上（见 ui_render.h）
            drawPicker();

            ui_commit();

            return APP_GTD;

        }



        if (g.mode == M_EDIT_FIELD) {

            auto &task = tasks[g.detailTaskIdx];

            auto &df = DETAIL_FIELDS[g.detailField];



            if (g.imeActive && key != 0) {

                std::string imeOut;

                if (imeFieldKeyText(g_ime, key, false, imeOut)) {

                    if (!imeOut.empty()) imeFieldInsert(gtdEditField(), imeOut);

                    drawDetail();

                    ui_commit();

                    return APP_GTD;

                }

            }

            if (key == KEY_IME_TOGGLE) {

                g.imeActive = !g.imeActive;

                g_ime.setActive(g.imeActive);

                drawDetail();

                ui_commit();

                return APP_GTD;

            }



            if (key == 0x1B) {

                g.mode = M_DETAIL;

                g.imeActive = false; g_ime.setActive(false);

            } else if ((key == 0x0A || key == 0x0D) && df.type == 'm') {

                // Multi-line: Enter inserts newline

                imeFieldInsert(gtdEditField(), "\n");

            } else if (key == '\t' && df.type == 'm') {

                // Tab commits multi-line note

                task.set(df.key, g.editBuf);

                saveData();

                rebuildFilter();

                g.mode = M_DETAIL;

                g.imeActive = false; g_ime.setActive(false);

            } else if ((key == 0x0A || key == 0x0D) && df.type != 'm') {

                // Commit field value

                if (df.type == 's')

                    task.set(df.key, g.editBuf);

                saveData();

                if (g.view == V_PROJECT) buildProjectList();

                rebuildFilter();

                g.mode = M_DETAIL;

                g.imeActive = false; g_ime.setActive(false);

            } else if (key == 0x7F || key == 0x08) {

                imeFieldBackspace(gtdEditField());

            } else if (key == KEY_LEFT) {

                imeFieldMoveLeft(gtdEditField());

            } else if (key == KEY_RIGHT) {

                imeFieldMoveRight(gtdEditField());

            } else if (key >= 0x20 && key <= 0x7E) {

                imeFieldInsert(gtdEditField(), std::string(1, (char)key));

            }

            drawDetail();

            ui_commit();

            return APP_GTD;

        }



        // M_DETAIL navigation

        if (key == 0x1B || key == 'q' || key == 'Q') {

            g.mode = M_BROWSE;

        } else if (key == KEY_UP || key == 'k') {

            if (g.detailField > 0) g.detailField--;

        } else if (key == KEY_DOWN || key == 'j') {

            if (g.detailField < NUM_DETAIL_FIELDS - 1) g.detailField++;

        } else if (key == 0x0A || key == 0x0D) {

            auto &df = DETAIL_FIELDS[g.detailField];

            auto &task = tasks[g.detailTaskIdx];

            // Open picker for selection types, calendar for date, text editor for string/number

            switch (df.type) {

            case 'p': case 't': case 'j': case 'c': case 'g': case 'n':

                openPicker(g.detailField);

                break;

            case 'd':

                openCalendar();

                break;

            case 's':

                g.editBuf = task[df.key].asString();

                g.editCur = (int)g.editBuf.length();

                g.imeActive = true;

                g_ime.setActive(true);

                g.mode = M_EDIT_FIELD;

                break;

            case 'm':

                openNoteEditor();

                break;

            }

        } else if (key == 's' || key == 'S') {

            g.summaryScroll = 0;

            g.summaryPrevMode = M_DETAIL;

            g.mode = M_SUMMARY;

            ui_render_begin_overlay();   // 叠在上一帧上（见 ui_render.h）
            drawSummary();

            return APP_GTD;

        } else if (key == '?') {

            g.helpScroll = 0;

            g.helpPrevMode = M_DETAIL;

            g.mode = M_HELP;

            ui_render_begin_overlay();   // 叠在上一帧上（见 ui_render.h）
            drawHelp();

            ui_commit();

            return APP_GTD;

        }



        drawDetail();

        ui_commit();

        return APP_GTD;

}

// 从 screen_gtd_handle 里原样搬出来的一个 g.mode 分支（见那边的分发表）。
static AppState gtdHandleTaxMgr(int key) {


        bool isCtx = (g.mode == M_CONTEXT_MGR);

        auto &list = isCtx ? g.contextList : g.tagList;

        int &sel = isCtx ? g.ctxMgrSel : g.tagMgrSel;

        const char *title = isCtx ? "情境管理" : "标签管理";

        char prefix = isCtx ? '@' : '#';



        if (key == 0x1B || key == 'q' || key == 'Q') {

            g.mode = M_BROWSE;

        } else if (key == KEY_UP || key == 'k') {

            if (sel > 0) sel--;

        } else if (key == KEY_DOWN || key == 'j') {

            if (sel < (int)list.size() - 1) sel++;

        } else if (key == 'a' || key == 'A') {

            g.mode = isCtx ? M_ADD_CONTEXT : M_ADD_TAG;

            g.editBuf.clear(); g.editCur = 0;

            g.imeActive = true; g_ime.setActive(true);

        } else if ((key == 'd' || key == 'D') && sel < (int)list.size()) {

            std::string name = list[sel];

            // Remove from stored list

            auto &arr = isCtx ? g.data["contexts"] : g.data["tags"];

            for (int i = (int)arr.size() - 1; i >= 0; i--)

                if (arr[i].asString() == name) arr.elements.erase(arr.elements.begin() + i);

            // Clear from tasks

            auto &tasks = g.data["tasks"];

            if (isCtx) {

                for (int i = 0; i < (int)tasks.size(); i++)

                    if (tasks[i]["context"].asString() == name) tasks[i].set("context", "");

                if (g.filterContext == name) g.filterContext.clear();

            } else {

                for (int i = 0; i < (int)tasks.size(); i++) {

                    auto &tt = tasks[i]["tags"];

                    if (tt.isArray()) {

                        for (int j = (int)tt.size() - 1; j >= 0; j--)

                            if (tt[j].asString() == name) tt.elements.erase(tt.elements.begin() + j);

                    }

                }

                for (int i = (int)g.filterTags.size() - 1; i >= 0; i--)

                    if (g.filterTags[i] == name) g.filterTags.erase(g.filterTags.begin() + i);

            }

            saveData();

            if (isCtx) buildContextList(); else buildTagList();

            if (sel >= (int)list.size()) sel = (int)list.size() - 1;

            if (sel < 0) sel = 0;

            rebuildFilter();

        } else if (key == 0x0A || key == 0x0D) {

            if (sel < (int)list.size()) {

                if (isCtx) {

                    g.filterContext = (g.filterContext == list[sel]) ? "" : list[sel];

                } else {

                    std::string name = list[sel];

                    bool found = false;

                    for (int i = 0; i < (int)g.filterTags.size(); i++) {

                        if (g.filterTags[i] == name) { g.filterTags.erase(g.filterTags.begin() + i); found = true; break; }

                    }

                    if (!found) g.filterTags.push_back(name);

                }

                rebuildFilter();

            }

        } else if (key == ' ' && !isCtx && sel < (int)list.size()) {

            // Tag multi-select toggle

            std::string name = list[sel];

            bool found = false;

            for (int i = 0; i < (int)g.filterTags.size(); i++) {

                if (g.filterTags[i] == name) { g.filterTags.erase(g.filterTags.begin() + i); found = true; break; }

            }

            if (!found) g.filterTags.push_back(name);

            rebuildFilter();

        } else if ((key == 'r' || key == 'R') && sel < (int)list.size()) {

            g.mode = isCtx ? M_RENAME_CONTEXT : M_RENAME_TAG;

            g.editBuf = list[sel];

            g.editCur = (int)g.editBuf.length();

            g.imeActive = true; g_ime.setActive(true);

            if (isCtx) g.renameTargetContext = list[sel];

            else g.renameTargetTag = list[sel];

        }



        // Draw

        ui_clear(); int y = FONT_H + 6 + LINE_SPACING;

        ui_draw_text(4, FONT_H, title, false, true);

        u8g2_DrawHLine(g_u8g2, 0, FONT_H + 4, SCREEN_W);

        int maxY = STATUS_Y;

        int vis = (maxY - y + LINE_SPACING - 1) / LINE_SPACING;

        if (vis < 1) vis = 1;

        if (sel < g.scroll) g.scroll = sel;

        if (sel >= g.scroll + vis) g.scroll = sel - vis + 1;



        for (int i = 0; i < vis && (g.scroll + i) < (int)list.size(); i++) {

            int idx = g.scroll + i;

            bool s = (idx == sel);

            char buf[64];

            snprintf(buf, sizeof(buf), "%c%s", prefix, list[idx].c_str());

            ui_draw_text(8, y + i * LINE_SPACING, buf, s);

            // Show filter indicator

            if (isCtx && g.filterContext == list[idx]) {

                g_font.drawText(SCREEN_W - g_font.textWidth("●") - 4, y + i * LINE_SPACING, "●", false);

            }

            if (!isCtx) {

                bool active = false;

                for (auto &ft : g.filterTags) if (ft == list[idx]) { active = true; break; }

                if (active) g_font.drawText(SCREEN_W - g_font.textWidth("●") - 4, y + i * LINE_SPACING, "●", false);

            }

        }

        if (list.empty()) ui_draw_text(8, y, "暂无 — 按a添加");



        // Show active filter

        char sl[96];

        if (isCtx)

            snprintf(sl, sizeof(sl), "a:添加 d:删除 r:重命名 Enter:筛选 Esc:返回 %d项", (int)list.size());

        else

            snprintf(sl, sizeof(sl), "a:添加 d:删除 r:重命名 Enter/Space:筛选 Esc:返回 %d项", (int)list.size());

        char rbuf[40]; gtdStatusRight(rbuf, sizeof(rbuf));
        ui_draw_status(sl, rbuf);

        drawIMEStatus(); ui_commit();

        return APP_GTD;

}

// 从 screen_gtd_handle 里原样搬出来的一个 g.mode 分支（见那边的分发表）。
static AppState gtdHandleTaxAdd(int key) {

        bool isCtx = (g.mode == M_ADD_CONTEXT);

        if (g.imeActive && key != 0) {

            std::string imeOut;

            if (imeFieldKeyText(g_ime, key, false, imeOut)) {

                if (!imeOut.empty()) imeFieldInsert(gtdEditField(), imeOut);

                drawAdd(); return APP_GTD;

            }

        }

        if (key == KEY_IME_TOGGLE) { g.imeActive = !g.imeActive; g_ime.setActive(g.imeActive); drawAdd(); return APP_GTD; }



        if (key == 0x1B) {

            g.mode = isCtx ? M_CONTEXT_MGR : M_TAG_MGR;

            g.imeActive = false; g_ime.setActive(false);

        } else if (key == 0x0A || key == 0x0D) {

            if (!g.editBuf.empty()) {

                std::string name = g.editBuf;

                if (!name.empty() && (name[0] == '@' || name[0] == '#')) name = name.substr(1);

                if (!name.empty()) {

                    auto &arr = isCtx ? g.data["contexts"] : g.data["tags"];

                    arr.pushBack(name);

                    saveData();

                    if (isCtx) buildContextList(); else buildTagList();

                }

            }

            g.mode = isCtx ? M_CONTEXT_MGR : M_TAG_MGR;

            g.imeActive = false; g_ime.setActive(false);

        } else if (key == 0x7F || key == 0x08) {

            imeFieldBackspace(gtdEditField());

        } else if (key >= 0x20 && key <= 0x7E) { imeFieldInsert(gtdEditField(), std::string(1, (char)key)); }

        drawAdd();

        return APP_GTD;

}

// 从 screen_gtd_handle 里原样搬出来的一个 g.mode 分支（见那边的分发表）。
static AppState gtdHandleSummary(int key) {

        if (key == 0x1B || key == 'q' || key == 'Q') {

            g.mode = g.summaryPrevMode;

        } else if (key == KEY_UP || key == 'k') {

            if (g.summaryScroll > 0) g.summaryScroll--;

            if (g.summaryPrevMode == M_BROWSE) { drawList(); drawSummary(); }
            else drawSummary();

            return APP_GTD;

        } else if (key == KEY_DOWN || key == 'j') {

            g.summaryScroll++;

            if (g.summaryPrevMode == M_BROWSE) { drawList(); drawSummary(); }
            else drawSummary();

            return APP_GTD;

        }

        // Re-draw underlying screen, then overlay summary

        if (g.summaryPrevMode == M_BROWSE) drawList();
        else drawDetail();

        drawSummary();

        return APP_GTD;

}

// 从 screen_gtd_handle 里原样搬出来的一个 g.mode 分支（见那边的分发表）。
static AppState gtdHandleArchive(int key) {

        if (key == 0x1B || key == 'q' || key == 'Q') {
            if (g.archiveBrowsing) {
                g.archiveBrowsing = false;
            } else {
                g.mode = M_BROWSE;
            }
        } else if (key == KEY_UP) {
            if (g.archiveBrowsing) {
                if (g.archiveViewSel > 0) g.archiveViewSel--;
            } else {
                if (g.archiveSel > 0) g.archiveSel--;
            }
        } else if (key == KEY_DOWN) {
            if (g.archiveBrowsing) {
                if (g.archiveViewSel < (int)g.archiveTasks.size() - 1) g.archiveViewSel++;
            } else {
                if (g.archiveSel < (int)g.archiveMonths.size() - 1) g.archiveSel++;
            }
        } else if ((key == 'd' || key == 'D') && !g.archiveBrowsing) {
            if (g.archiveSel >= 0 && g.archiveSel < (int)g.archiveMonths.size()) {
                std::string path = std::string(ARCHIVE_DIR) + "/" + g.archiveMonths[g.archiveSel] + ".json";
                remove(path.c_str());
                g.archiveMonths.erase(g.archiveMonths.begin() + g.archiveSel);
                g.archiveCounts.erase(g.archiveCounts.begin() + g.archiveSel);
                if (g.archiveSel >= (int)g.archiveMonths.size()) g.archiveSel = (int)g.archiveMonths.size() - 1;
                if (g.archiveSel < 0) g.archiveSel = 0;
            }
        } else if ((key == 0x0A || key == 0x0D) && !g.archiveBrowsing) {
            if (g.archiveSel >= 0 && g.archiveSel < (int)g.archiveMonths.size()) {
                g.archiveViewMonth = g.archiveMonths[g.archiveSel];
                loadArchiveMonthTasks(g.archiveViewMonth);
                g.archiveBrowsing = true;
                g.archiveViewSel = 0;
                g.archiveViewScroll = 0;
            }
        } else if (key == '?' || key == 'h') {
            g.helpScroll = 0;
            g.helpPrevMode = M_ARCHIVE;
            g.mode = M_HELP;
            ui_render_begin_overlay();   // 叠在上一帧上（见 ui_render.h）
            drawHelp();
            ui_commit();
            return APP_GTD;
        }

        if (g.mode != M_HELP) drawArchiveMgr();
        return APP_GTD;
}

// 从 screen_gtd_handle 里原样搬出来的一个 g.mode 分支（见那边的分发表）。
static AppState gtdHandleHelp(int key) {

        if (key == 0x1B || key == 'q' || key == 'Q' || key == 0x0A || key == 0x0D) {

            g.mode = g.helpPrevMode;

        } else if (key == KEY_UP || key == 'k') {

            if (g.helpScroll > 0) g.helpScroll--;

        } else if (key == KEY_DOWN || key == 'j') {

            g.helpScroll++;

        } else if (key == KEY_LEFT) {

            g.helpScroll -= 5;

            if (g.helpScroll < 0) g.helpScroll = 0;

        } else if (key == KEY_RIGHT) {

            g.helpScroll += 5;

        }

        ui_render_begin_overlay();   // 叠在上一帧上（见 ui_render.h）
        drawHelp();

        ui_commit();

        return APP_GTD;

}

// 从 screen_gtd_handle 里原样搬出来的一个 g.mode 分支（见那边的分发表）。
static AppState gtdHandleNote(int key, ScreenContext &ctx, bool hasTap) {
    auto &tasks = g.data["tasks"];


        // 触摸编辑（共享件 text_sel）：长按备注行 → 复制/剪切/粘贴/全选。
        // **排在 IME 分支之前**：会话靠这里吃掉拖动/长按键，免得漏进 g_ime（有未上屏
        // 组合时未知键会上屏候选字）；也只有排在这儿，"全选后直接打字"才能先把选区
        // 删掉、再把字符放给 IME 插到新光标位置。
        if (key == KEY_TOUCH_LONG || textSelActive()) {
            int tx = 0, ty = 0;
            const bool hasTap = input_tap_xy(&tx, &ty);
            noteFlatten();
            int flatCur = noteFlatCursor();
            std::vector<TextSelLine> tl;
            int bottom = 0;
            noteEditorLineTable(tl, bottom);
            TextSelView view{tl.data(), (int)tl.size(), bottom};
            if (key == KEY_TOUCH_LONG) {
                if (hasTap && textSelBegin(s_noteFlat, flatCur, view, tx, ty)) {
                    g_ime.cancelComposition();
                    noteUnflatten(flatCur);
                    drawNoteEditor();
                    return APP_GTD;
                }
                key = 0x1B;   // 没落在备注行上 → 维持"长按 = 返回"的老语义
            } else if (key != 0) {
                const bool consumed =
                    textSelHandleKey(s_noteFlat, flatCur, view, key, tx, ty, hasTap,
                                     &ctx.statusMessage);
                // 不论消费与否都同步回去：会话可能改过缓冲（剪切/粘贴/退格删选区），
                // 未消费时（如"全选后直接打字"）光标也得跟到选区起点，字符才插对地方。
                noteUnflatten(flatCur);
                if (consumed) {
                    drawNoteEditor();
                    return APP_GTD;
                }
            }
        }

        if (g.imeActive && key != 0) {

            std::string imeOut;

            if (imeFieldKeyText(g_ime, key, false, imeOut)) {

                if (!imeOut.empty()) { imeFieldInsert(gtdNoteField(), imeOut); g.noteVrowsDirty = true; }

                drawNoteEditor();

                return APP_GTD;

            }

        }

        if (key == KEY_IME_TOGGLE) {

            g.imeActive = !g.imeActive;

            g_ime.setActive(g.imeActive);

            drawNoteEditor();

            return APP_GTD;

        }



        if (key == 0x1B) {

            // Cancel

            g.mode = M_DETAIL;

            g.imeActive = false; g_ime.setActive(false);

        } else if (key == 0x13 || key == KEY_CTRL_ENTER) {

            // Ctrl+S or Ctrl+Enter — save

            std::string text;

            for (size_t i = 0; i < g.noteLines.size(); i++) {

                if (i > 0) text += '\n';

                text += g.noteLines[i];

            }

            auto &task = tasks[g.detailTaskIdx];

            task.set("note", text);

            saveData();

            g.mode = M_DETAIL;

            g.imeActive = false; g_ime.setActive(false);

        } else if (key == 0x0A || key == 0x0D) {

            // Enter — insert newline

            std::string rest = g.noteLines[g.noteRow].substr(g.noteCol);

            g.noteLines[g.noteRow].erase(g.noteCol);

            g.noteLines.insert(g.noteLines.begin() + g.noteRow + 1, rest);

            g.noteRow++;

            g.noteCol = 0;

            g.noteVrowsDirty = true;

        } else if (key == 0x7F || key == 0x08) {

            // Backspace

            if (g.noteCol > 0) {

                imeFieldBackspace(gtdNoteField());

                g.noteVrowsDirty = true;

            } else if (g.noteRow > 0) {

                // Join with previous line

                g.noteCol = (int)g.noteLines[g.noteRow - 1].length();

                g.noteLines[g.noteRow - 1] += g.noteLines[g.noteRow];

                g.noteLines.erase(g.noteLines.begin() + g.noteRow);

                g.noteRow--;

                g.noteVrowsDirty = true;

            }

        } else if (key == KEY_UP || key == 'k') {

            if (g.noteRow > 0) {

                g.noteRow--;

                if (g.noteCol > (int)g.noteLines[g.noteRow].length())

                    g.noteCol = (int)g.noteLines[g.noteRow].length();

            }

        } else if (key == KEY_DOWN || key == 'j') {

            if (g.noteRow < (int)g.noteLines.size() - 1) {

                g.noteRow++;

                if (g.noteCol > (int)g.noteLines[g.noteRow].length())

                    g.noteCol = (int)g.noteLines[g.noteRow].length();

            }

        } else if (key == KEY_LEFT) {

            if (g.noteCol > 0) {

                imeFieldMoveLeft(gtdNoteField());

            } else if (g.noteRow > 0) {

                g.noteRow--;

                g.noteCol = (int)g.noteLines[g.noteRow].length();

            }

        } else if (key == KEY_RIGHT) {

            if (g.noteCol < (int)g.noteLines[g.noteRow].length()) {

                imeFieldMoveRight(gtdNoteField());

            } else if (g.noteRow < (int)g.noteLines.size() - 1) {

                g.noteRow++;

                g.noteCol = 0;

            }

        } else if (key >= 0x20 && key <= 0x7E) {

            imeFieldInsert(gtdNoteField(), std::string(1, (char)key));

            g.noteVrowsDirty = true;

        }

        drawNoteEditor();

        return APP_GTD;

}

// 从 screen_gtd_handle 里原样搬出来的一个 g.mode 分支（见那边的分发表）。
static AppState gtdHandleFilter(int key) {

        if (g.imeActive && key != 0) {

            std::string imeOut;

            if (imeFieldKeyText(g_ime, key, false, imeOut)) {

                if (!imeOut.empty()) imeFieldInsert(gtdFilterField(), imeOut);

                rebuildFilter();

                drawList();

                ui_commit();

                return APP_GTD;

            }

        }

        if (key == KEY_IME_TOGGLE) {

            g.imeActive = !g.imeActive;

            g_ime.setActive(g.imeActive);

            drawList();

            ui_commit();

            return APP_GTD;

        }

        if (key == 0x1B) {

            g.mode = M_BROWSE;

            g.imeActive = false; g_ime.setActive(false);

            g.filterText.clear();

            rebuildFilter();

        } else if (key == 0x0A || key == 0x0D) {

            g.mode = M_BROWSE;

            g.imeActive = false; g_ime.setActive(false);

        } else if (key == 0x7F || key == 0x08) {

            if (imeFieldBackspace(gtdFilterField())) rebuildFilter();

        } else if (key >= 0x20 && key <= 0x7E) {

            imeFieldInsert(gtdFilterField(), std::string(1, (char)key));

            rebuildFilter();

        }

        drawList();

        ui_commit();

        return APP_GTD;

}

// 从 screen_gtd_handle 里原样搬出来的 M_BROWSE 主体（列表交互 + 落尾重绘）。
static AppState gtdHandleBrowse(int key, ScreenContext &ctx) {
    auto &tasks = g.data["tasks"];

    // ── M_BROWSE: main list view ─────────────────────────────────────

    // 手指按住上下拖 = 按像素平滑滚动列表。增量由 hw/input 按帧上报，这里滚完
    // 就落到函数尾重绘（ui_commit 只发有变化的部分）。
    if (key == KEY_TOUCH_DRAG) {
        int ddx = 0, ddy = 0;
        if (input_drag_xy(&ddx, &ddy)) {
            gtdScrollByPx(ddy);
            s_dragConsumed = true;
        }
    }

    // 任何非 Shift 多选/删除/空闲按键都清除多选状态（拖动不清——拖动只是滚屏）
    if (key != 0 && key != KEY_SHIFT_UP && key != KEY_SHIFT_DOWN && key != 'd' && key != 'D'
        && key != KEY_TOUCH_DRAG) {
        clearMultiSel();
    }

    // 长按列表项 → 弹编辑菜单（重命名/删除）。长按的点落在哪一行就编辑哪一行，
    // 与键盘"先选中再按 r/d"完全等价：把 g.sel 指过去，菜单里的动作走同一对入口。
    if (key == KEY_TOUCH_LONG) {
        int lx = 0, ly = 0;
        if (input_tap_xy(&lx, &ly)) {
            int rows = gtdListRows();
            int r = gtdRowAtY(gtdFirstRowY() - g.scrollFrac, ly, rows);
            if (r >= 0) {
                int idx = g.scroll + r;
                // 项目树视图的 g.scroll/g.sel 是"可见树结点"下标，与 g.filtered 不是
                // 一套，但这种布局下 filtered 是按树序重建的，所以下标仍然对齐。
                if (idx >= 0 && idx < rows) {
                    g.sel = idx;
                    g.itemMenuSel = 0;
                    g.mode = M_ITEM_MENU;
                    s_dragConsumed = false;
                    ui_render_begin_overlay();   // 叠在上一帧上（见 ui_render.h）
                    drawItemMenu();
                    return APP_GTD;
                }
            }
        }
    }

    // Project list management (only in project list view)

    if (isInProjectList()) {

        if (key == 'n' || key == 'N') {

            g.mode = M_ADD_PROJECT;

            g.editBuf.clear();

            g.editCur = 0;

            g.imeActive = true;

            g_ime.setActive(true);

        }

        // 删除/重命名项目：与长按菜单共用同一条入口，两条路不会跑偏。
        if ((key == 'd' || key == 'D') && g.sel < (int)g.projectList.size()) {

            gtdBeginDelete(ctx);

        }

        if ((key == 'r' || key == 'R') && g.sel < (int)g.projectList.size()) {

            gtdBeginRename();

        }

    }



    // 计划模式是独立的一档模式（电源键在 阅读/写作/计划 之间轮换），Esc/返回
    // 只在本模式内层层退回，最终停在计划模式主界面，不再掉回写作模式。
    if (key == 'q' || key == 'Q' || key == 0x1B) {

        if (hasMultiSel()) {          // 先取消多选，再谈返回

            clearMultiSel();

            return APP_GTD;

        }

        if (g.view == V_PROJECT && g.projectDrillIdx >= 0) {   // 项目内 → 项目列表

            gtdSetView(V_PROJECT);    // 复位选中/滚动/折叠，回到项目列表

            return APP_GTD;

        }

        if (!g.filterContext.empty() || !g.filterTags.empty()) {

            g.filterContext.clear();

            g.filterTags.clear();

            rebuildFilter();

            return APP_GTD;

        }

        // 已在主界面最外层：留在计划模式。这里原本弹一条"电源键切换模式"的提示，
        // 但主界面按返回是高频动作，每次都蹦提示反而吵，直接静默留着即可。
        return APP_GTD;

    }



    if (key == '\t') {

        gtdSetView(g.view + 1);

    }

    // 上下滑 = 整页翻列表（main.cpp 对计划模式保留了 PAGE_UP/DOWN，不降级成单步），
    // 左右滑 = 切换标签页（触屏没有 Tab 键）。
    if (key == KEY_PAGE_UP || key == KEY_PAGE_DOWN) {

        bool afterDrag = s_dragConsumed && !g_key_from_ble;

        s_dragConsumed = false;

        if (!afterDrag) gtdScrollByPage(key == KEY_PAGE_DOWN ? 1 : -1);

    }

    if (key == KEY_LEFT) gtdSetView(g.view - 1);

    if (key == KEY_RIGHT) gtdSetView(g.view + 1);



    if (key == KEY_UP || key == KEY_DOWN || key == KEY_HOME || key == KEY_END) {
        ListView lv;
        lv.sel = g.sel;
        lv.first = g.scroll;
        // count/rows 要跟 drawList 画的是同一份：项目列表视图画的是 g.projectList、
        // 下钻画的是 g_visibleTreeIdx，都不是 g.filtered（旧写法用 filtered.size() 当上界，
        // 在这两个视图下会把选中夹到错的长度）。gtdListRows() 就是"当前视图总行数"，
        // 与 gtdScrollByPage/滚动条口径一致；rows 必须是**可见行数** —— 传成总行数会让
        // listViewFollow 的 maxFirst 恒为 0，窗口再也滚不动（这正是"统一走 ListView"那笔
        // 引入的：四个键都吃掉、sel 动、scroll 不动）。
        lv.count = gtdListRows();
        lv.rows = gtdListVisibleRows();
        if (listViewKey(lv, key)) {
            listViewFollow(lv);
            g.sel = lv.sel;
            g.scroll = lv.first;
            g.scrollFrac = 0;
        }
    }

    // Shift+↑/↓ 连续多选(仅平铺列表; 项目列表/项目树显示顺序与 filtered 不一致)
    if (g.view != V_PROJECT) {

        if (key == KEY_SHIFT_UP) {

            if (g.multiAnchor < 0) g.multiAnchor = g.sel;

            if (g.sel > 0) g.sel--;

            rebuildMultiSel();

        }

        if (key == KEY_SHIFT_DOWN) {

            if (g.multiAnchor < 0) g.multiAnchor = g.sel;

            if (g.sel < (int)g.filtered.size() - 1) g.sel++;

            rebuildMultiSel();

        }

    }



    if (key == 'a' || key == 'A') {

        g.mode = M_ADD;

        g.editBuf.clear();

        g.editCur = 0;

        g.imeActive = true;

        g_ime.setActive(true);

        // same-level: inherit parent and project from selected task, insert after

        if (g.sel < (int)g.filtered.size()) {

            g.pendingParent = tasks[g.filtered[g.sel]]["parent"].asString();

            g.insertAfter = g.filtered[g.sel];

        } else {

            g.pendingParent.clear();

            g.insertAfter = -1;

        }

        // In project drill-down, auto-set project for new task

        if (g.view == V_PROJECT && g.projectDrillIdx >= 0 && g.projectDrillIdx < (int)g.projectList.size()) {

            g.pendingProject = g.projectList[g.projectDrillIdx];

        } else {

            g.pendingProject.clear();

        }

    }



    if (key == ' ' && g.sel < (int)g.filtered.size()) {

        auto &task = tasks[g.filtered[g.sel]];

        std::string s = task["status"].asString("todo");

        if (s == "todo") task.set("status", "doing");

        else if (s == "doing") { task.set("status", "done");

            time_t n; time(&n); struct tm *tm = localtime(&n);

            char b[16]; strftime(b, sizeof(b), "%Y-%m-%d", tm);

            task.set("completed", std::string(b));

        }

        else if (s == "done") task.set("status", "waiting");

        else task.set("status", "todo");

        saveData();

        rebuildFilter();

    }



    if (key == 0x0A || key == 0x0D) {

        if (isInProjectList()) {

            // Project list: drill into selected project

            if (g.sel < (int)g.projectList.size()) {

                g.projectDrillIdx = g.sel;

                g.sel = 0; g.scroll = 0;
                g.foldedNodes.clear();

                rebuildFilter();

            }

        } else if (g.sel < (int)g.filtered.size()) {

            // All tabs: open detail panel

            g.detailTaskIdx = g.filtered[g.sel];

            g.detailField = 0;

            g.mode = M_DETAIL;

            drawDetail();

            ui_commit();

            return APP_GTD;

        }

    }



    if ((key == 'i' || key == 'I') && g.sel < (int)g.filtered.size()) {

        // add subtask under selected, insert after parent

        auto &parentTask = tasks[g.filtered[g.sel]];

        g.pendingParent = parentTask["id"].asString();

        g.insertAfter = g.filtered[g.sel];

        // In project drill-down, auto-set project

        if (g.view == V_PROJECT && g.projectDrillIdx >= 0 && g.projectDrillIdx < (int)g.projectList.size())

            g.pendingProject = g.projectList[g.projectDrillIdx];

        else

            g.pendingProject.clear();

        g.mode = M_ADD;

        g.editBuf.clear();

        g.editCur = 0;

        g.imeActive = true;

        g_ime.setActive(true);

    }



    if (key == '/') {

        g.mode = M_FILTER;

        g.imeActive = true;

        g_ime.setActive(true);

    }



    if (key == 'A') {

        mkdir(ARCHIVE_DIR, 0755);

        loadArchiveMonths();

        g.archiveSel = 0;

        g.archiveScroll = 0;

        g.archiveBrowsing = false;

        g.mode = M_ARCHIVE;

    }



    if (key == 'c' || key == 'C') {

        buildContextList();

        g.ctxMgrSel = 0;

        g.mode = M_CONTEXT_MGR;

    }



    if (key == 't' || key == 'T') {

        buildTagList();

        g.tagMgrSel = 0;

        g.mode = M_TAG_MGR;

    }

    // s: open summary dialog
    if ((key == 's' || key == 'S') && !isInProjectList() && g.sel >= 0 && g.sel < (int)g.filtered.size()) {
        g.detailTaskIdx = g.filtered[g.sel];
        g.summaryScroll = 0;
        g.summaryPrevMode = M_BROWSE;
        g.mode = M_SUMMARY;
        drawList(); drawSummary();
        return APP_GTD;
    }

    // hjkl: reorder and hierarchy adjustment

    if (!isInProjectList() && g.view != V_COMPLETED && g.sel >= 0 && g.sel < (int)g.filtered.size()) {

        auto &ts = g.data["tasks"];

        if (key == 'j' && g.sel > 0) {

            // Move task up in order (swap with above)

            int a = g.filtered[g.sel];

            int b = g.filtered[g.sel - 1];

            std::swap(ts.elements[a], ts.elements[b]);

            g.sel--;

            // 项目详情列表画的是**树序**（g_gtdTree 由 g.filtered 的槽位顺序生成），
            // 换过槽位内容后必须重建：否则树线/缩进还按旧内容画，行看着是错位的
            // （键盘 j/k 在这条路径上原本也吃亏，一并修掉）。
            if (g.view == V_PROJECT && g.projectDrillIdx >= 0) rebuildFilter();

            saveData();

        } else if (key == 'k' && g.sel < (int)g.filtered.size() - 1) {

            // Move task down in order (swap with below)

            int a = g.filtered[g.sel];

            int b = g.filtered[g.sel + 1];

            std::swap(ts.elements[a], ts.elements[b]);

            g.sel++;

            if (g.view == V_PROJECT && g.projectDrillIdx >= 0) rebuildFilter();

            saveData();

        } else if (key == 'h') {
            // Promote: clear parent (increase hierarchy level)
            ts[g.filtered[g.sel]].set("parent", "");
            saveData();
            rebuildFilter();
        } else if (key == 'l') {
            // Demote: find previous sibling (same parent) and make current its child
            auto &task = ts[g.filtered[g.sel]];
            std::string myParent = task["parent"].asString();
            int sibIdx = -1;
            for (int i = g.sel - 1; i >= 0; i--) {
                if (ts[g.filtered[i]]["parent"].asString() == myParent) {
                    sibIdx = g.filtered[i];
                    break;
                }
            }
            if (sibIdx >= 0) {
                task.set("parent", ts[sibIdx]["id"].asString());
                saveData();
                rebuildFilter();
            }

        }

    }



    // z: toggle fold, Z: fold/unfold all (project drill-down only)
    if (g.view == V_PROJECT && g.projectDrillIdx >= 0 && !g_visibleTreeIdx.empty()) {
        if (key == 'z' && g.sel >= 0 && g.sel < (int)g_visibleTreeIdx.size()) {
            int treeIdx = g_visibleTreeIdx[g.sel];
            // Check if has children
            if (treeIdx + 1 < (int)g_gtdTree.size() && g_gtdTree[treeIdx + 1].depth > g_gtdTree[treeIdx].depth) {
                if (g.foldedNodes.count(treeIdx)) g.foldedNodes.erase(treeIdx);
                else g.foldedNodes.insert(treeIdx);
                rebuildFilter();
            }
        }
        if (key == 'Z') {
            if (g.foldedNodes.empty()) {
                // Fold all nodes that have children
                for (int i = 0; i < (int)g_gtdTree.size(); i++) {
                    if (i + 1 < (int)g_gtdTree.size() && g_gtdTree[i + 1].depth > g_gtdTree[i].depth)
                        g.foldedNodes.insert(i);
                }
            } else {
                g.foldedNodes.clear();
            }
            rebuildFilter();
        }
    }



    if (key == 'r' || key == 'R') {

        if (gtdBeginRename()) {

            drawAdd();  // reuse add-task overlay for rename

            ui_commit();

            return APP_GTD;

        }

    }



    if (key == 0x05) {  // Ctrl+E — export

        std::string md = exportMD();

        time_t now; time(&now); struct tm *tm = localtime(&now);

        char fname[64];

        strftime(fname, sizeof(fname), "/sdcard/gtd/gtd_export_%Y%m%d_%H%M%S.md", tm);

        g_journal.saveEntryRaw(std::string(fname).c_str() + 12, md);

        ctx.statusMessage = "已导出Markdown";

    }



    if ((key == 'd' || key == 'D') && g.sel < (int)g.filtered.size()) {

        if (g.view != V_PROJECT && hasMultiSel()) {

            // 批量删除: 收集选中 id 对应的任务下标
            std::vector<int> idxs;
            for (int i = 0; i < (int)tasks.size(); i++) {
                if (g.multiSel.count(tasks[i]["id"].asString()) > 0) idxs.push_back(i);
            }
            if (!idxs.empty()) {
                g.confirmIdxs = idxs;
                g.confirmMsg = "删除所选 " + std::to_string((int)idxs.size()) + " 个任务?";
                g.mode = M_CONFIRM;
            }

        } else {

            gtdBeginDelete(ctx);   // 与长按菜单同一条路

        }

    }



    if (key == '?') {

        g.helpScroll = 0;

        g.helpPrevMode = M_BROWSE;

        g.mode = M_HELP;

        ui_render_begin_overlay();   // 叠在上一帧上（见 ui_render.h）
        drawHelp();

        ui_commit();

        return APP_GTD;

    }



    drawList();

    ui_commit();

    return APP_GTD;

}


AppState screen_gtd_handle(int key, ScreenContext &ctx) {

    // 切进"要打字"的模式（新建/重命名/编辑字段/写笔记/筛选）时，自动把虚拟键盘
    // 弹出来（没连蓝牙键盘的话）。只在**模式切换**的那一刻自动展示：用户在本模式里
    // 手动点状态栏图标收起后，不会被下一帧又弹回来。
    static int s_prevMode = M_BROWSE;
    if (g.mode != s_prevMode) {
        if (gtdVkEditing()) editorVkAutoShow();
        // 换了模式 = 换了字段：上一个字段的选区 / 按钮条作废，
        // 免得把 A 的选区贴到 B 上（也顺带把 g.field* 的旧值挤掉）。
        textSelReset();
        g.fieldX = 0;
    }
    s_prevMode = g.mode;


    // 点按坐标只读一次：下面键盘拦截和一般分流都要用，读第二遍就空了。
    int tapX = 0, tapY = 0;
    bool hasTap = ((key == 0x0A || key == 0x0D) && input_tap_xy(&tapX, &tapY));

    // 虚拟键盘（编辑态，与写作模式同一套）：先认状态栏上的键盘开关图标，再认
    // 键盘面板。命中就把点按翻译成普通键码（或翻转待发状态），交给下面既有的
    // 输入逻辑——不重复实现任何输入。key 置 0 时各编辑块照旧重绘，正好刷出反馈。
    if (gtdVkEditing()) editorVkSyncBtState();   // 蓝牙键盘连上就自动收起
    if (hasTap && gtdVkEditing()) {
        if (editorVkIconHit(tapX, tapY)) {
            editorVkSetVisible(!editorVkVisible());
            hasTap = false; key = 0;
        } else if (editorVkVisible() && tapY >= editorVkTop()) {
            EditorVkHit hit;
            int k = editorVkHitTest(tapX, tapY, &hit);
            hasTap = false; key = 0;
            // 按下反馈：记下命中键，反色随下面的重绘一起上屏（不额外推屏）。
            if (k != EVK_NONE) editorVkMarkPressed(hit);
            // EVK_PAGE：换面板已在命中测试里完成，key 保持 0 → 下面照样重绘
            if (k == EVK_LANG) {
                // 未开输入法 → 开中文；已开 → 拼音/英文互切（与物理 Ctrl+Space 等价）
                if (!g.imeActive) { g.imeActive = true; g_ime.setActive(true); }
                else g_ime.toggleEnglish();
            } else if (k > 0) {
                key = k;
            }
            // EVK_CTRL/EVK_SHIFT：待发状态已在命中测试内翻转，重绘即反馈
            // EVK_NONE：点在键盘空白处，吞掉本次点按
        }
    }

    // T9 候选面板里的上下滑（与写作/阅读同一套实现）：左列滚读音、宫格翻候选页。
    // 必须放在下面把 PAGE_UP/DOWN 折成 UP/DOWN 之前——那一折是给子界面的单步移动用的，
    // 滚面板不能走那条路；否则滚一下面板会顺带把正文/列表也挪一格。
    if ((key == KEY_PAGE_UP || key == KEY_PAGE_DOWN) && gtdVkActive()) {
        int px = 0, py = 0;
        if (input_press_xy(&px, &py) &&
            editorVkSwipeScroll(px, py, key == KEY_PAGE_DOWN ? +1 : -1)) {
            key = 0;   // 这一划归面板；各编辑块照旧重绘，正好把滚动的结果刷出去
        }
    }

    // ── 共享的单行输入框触摸编辑（长按选字 / 复制 / 剪切 / 粘贴 / 全选）──────
    // **排在虚拟键盘之后**：键盘上的点按优先（上面已经翻成普通键码了）——
    // 会话开着时在键盘上敲字，会先在这里吃掉选区（"全选后直接打字"就地替换）。
    // **排在下面的坐标分流之前**：按钮条落在正文区中部，若先过 gtdKeyFromTap 会被
    // 当成"点列表空白"翻成 0，这一按就白点了。
    {
        std::string *fb = nullptr;
        int *fc = nullptr;
        TextSelLine fln;
        TextSelView fview;
        if (gtdFlatField(&fb, &fc, &fln, &fview)) {
            if (key == KEY_TOUCH_LONG) {
                int lx = 0, ly = 0;
                if (input_tap_xy(&lx, &ly) && textSelBegin(*fb, *fc, fview, lx, ly)) {
                    g_ime.cancelComposition();   // 别让未上屏的组合悬着
                    gtdRedrawFieldMode();
                    return APP_GTD;
                }
                key = 0x1B;   // 没落在输入行上 → 维持"长按 = 返回"的老语义
            } else if (key != 0 &&
                       textSelHandleKey(*fb, *fc, fview, key, tapX, tapY, hasTap,
                                        &ctx.statusMessage)) {
                gtdRedrawFieldMode();
                return APP_GTD;
            }
        } else if (textSelActive() && g.mode != M_EDIT_NOTE) {
            // 字段没了（提交/取消/切视图）→ 收起会话。备注编辑器是**多行**宿主，
            // 走下面自己那一套（拍平镜像），别在这儿把它的会话误收掉。
            textSelReset();
        }
    }

    // 触摸点按再按坐标分流：命中标签栏/列表行/详情栏/选择器项/悬浮「+」时，
    // gtdKeyFromTap 会把它换成对应的键（或就地处理），下面的键盘逻辑照旧执行；
    // 没命中的点按保持 '\n'，仍旧是原来的 Enter 语义。
    if (hasTap) {
        int k = gtdKeyFromTap(tapX, tapY);
        if (k >= 0) key = k;
    }

    // main.cpp 把触摸上下滑的翻页键原样留给计划模式（列表要整页翻），
    // 其余子界面（添加/详情/选择器/日历…）这里退回单步，保持原有手感。
    if (g.mode != M_BROWSE) {
        if (key == KEY_PAGE_UP) key = KEY_UP;
        else if (key == KEY_PAGE_DOWN) key = KEY_DOWN;
        // 拖动增量只对列表有意义；子界面把它连同增量一起丢掉，
        // 免得这个新键码流进输入法/编辑逻辑里。
        if (key == KEY_TOUCH_DRAG) {
            input_drag_xy(nullptr, nullptr);
            key = 0;
        }
    }



    // ── 按 g.mode 分发 ──────────────────────────────────────────────────
    // 每一块原来都是这里的一段 `if (g.mode == …) { … return APP_GTD; }`，现在整体搬进
    // 下面同名函数（P3c）。**逻辑一行没改**，所有分支都返回 APP_GTD，所以这里是
    // 一张纯分发表；没命中的落到 M_BROWSE 主体。
    if (g.mode == M_ADD) return gtdHandleAdd(key);
    if (g.mode == M_RENAME) return gtdHandleRename(key);
    if (g.mode == M_ITEM_MENU) return gtdHandleItemMenu(key, ctx);
    if (g.mode == M_CONFIRM) return gtdHandleConfirm(key, ctx);
    if (g.mode == M_ADD_PROJECT || g.mode == M_RENAME_PROJECT ||
        g.mode == M_RENAME_CONTEXT || g.mode == M_RENAME_TAG)
        return gtdHandleTaxNameEdit(key);
    if (g.mode == M_DETAIL || g.mode == M_EDIT_FIELD || g.mode == M_PICKER ||
        g.mode == M_CALENDAR)
        return gtdHandleDetailViews(key);
    if (g.mode == M_CONTEXT_MGR || g.mode == M_TAG_MGR) return gtdHandleTaxMgr(key);
    if (g.mode == M_ADD_CONTEXT || g.mode == M_ADD_TAG) return gtdHandleTaxAdd(key);
    if (g.mode == M_SUMMARY) return gtdHandleSummary(key);
    if (g.mode == M_ARCHIVE) return gtdHandleArchive(key);
    if (g.mode == M_HELP) return gtdHandleHelp(key);
    if (g.mode == M_EDIT_NOTE) return gtdHandleNote(key, ctx, hasTap);
    if (g.mode == M_FILTER) return gtdHandleFilter(key);
    return gtdHandleBrowse(key, ctx);
}
