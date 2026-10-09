#include "screen_outline.h"
#include "editor_vk.h"  // 编辑态虚拟键盘（与写作/计划模式同一套）
#include "font_renderer.h"
#include "hw/input.h"   // input_tap_xy：触摸点按（悬浮 + 键）
#include "json_parser.h"
#include "journal_storage.h"
#include "ui_helpers.h"
#include "ui/list_view.h"  // listPageStep：手写列表的翻页步长（一屏行数 / 没得翻 = 0）
#include "ui_render.h"
#include "ime/IME.h"
#include "ui/ime_field.h"  // 输入框绑定：落串/退格/光标的 UTF-8 算术（与写作/计划/阅读共用一份）
#include <cstdio>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <sys/stat.h>
#include <algorithm>
#include <unistd.h>
#include <set>
#include "u8g2_shim.h"

#define KEY_CTRL_ENTER 0x85
static const long MAX_OUTLINE_CONTENT_FILE_SIZE = 256 * 1024;

// ── File icon bitmap (from Go-Song2Propo-NF-R.ttf, U+F15B fa-file-text-o) ──
static const uint8_t FILE_ICON_BITS[] = {
    0x00, 0x00, 0x7E, 0x80, 0xFE, 0xC0, 0xFE, 0xE0, 0xFE, 0xF0, 0xFF, 0x00,
    0xFF, 0xF8, 0xFF, 0xF8, 0xFF, 0xF8, 0xFF, 0xF8, 0xFF, 0xF8, 0xFF, 0xF8,
    0xFF, 0xF8, 0xFF, 0xF8, 0xFF, 0xF8, 0xFF, 0xF8, 0xFF, 0xF0, 0x3F, 0xE0,
};
#define FILE_ICON_W 13
#define FILE_ICON_H 18
#define FILE_ICON_ROW_BYTES 2

static void drawFileIcon(int x, int baseline) {
    int top = baseline - FILE_ICON_H;
    for (int row = 0; row < FILE_ICON_H; row++) {
        for (int col = 0; col < FILE_ICON_W; col++) {
            int bi = row * FILE_ICON_ROW_BYTES + col / 8;
            int bit = 7 - (col % 8);
            if (FILE_ICON_BITS[bi] & (1 << bit))
                u8g2_DrawPixel(g_u8g2, x + col, top + row);
        }
    }
}

#define OUTLINE_DIR "/sdcard/outline"

// Outline node status values
static const char *OUTLINE_STATUS[] = {"draft", "active", "done", "revise"};
static const char *OUTLINE_STATUS_DISPLAY[] = {"草稿", "进行中", "已完成", "待修改"};
static const int OUTLINE_STATUS_COUNT = 4;

enum Mode { M_PROJECTS, M_BROWSE, M_DETAIL, M_ADD_PROJECT, M_ADD_HEADING, M_ADD_SUB, M_FILTER, M_EDIT_NOTE, M_EDIT_NOTE_ML, M_SUMMARY, M_HELP, M_BOOKMARK_MGR, M_CONFIRM, M_TAG_MGR, M_ADD_TAG, M_RENAME_TAG, M_PICKER, M_ITEM_MENU };

// ── State ────────────────────────────────────────────────────────────────
static struct {
    int mode = M_PROJECTS;
    int sel = 0;
    int scroll = 0;

    // project list
    std::vector<std::string> projects;
    int curProject = -1;     // index into projects

    // current project outline data
    JsonValue outlineData;   // { nodes: [...] }
    std::vector<JsonValue> *nodes = nullptr;
    size_t nodeCount = 0;

    // editing
    std::string editBuf;
    int editCur = 0;
    bool imeActive = false;
    int pendingLevel = 0;    // heading level for next add
    int insertAfter = -1;    // index in nodes to insert after, -1 = append

    // filter
    std::string filterText;
    std::vector<std::string> filterTags;

    // heading whose note is being edited (index into nodes)
    int editNoteIdx = -1;
    bool editingTitle = false;
    bool editingKeyword = false;

    // detail view
    int detailNodeIdx = -1;
    int detailField = 0;

    // summary dialog
    int summaryScroll = 0;
    int summaryNodeIdx = -1;

    // fold state
    std::set<int> foldedNodes;  // indices of folded (collapsed) nodes

    // bookmark state
    int bmMgrSel = 0;

    // multi-line note editor
    std::vector<std::string> noteLines;
    int noteRow = 0;
    int noteCol = 0;
    int noteScroll = 0;
    std::vector<VRow> noteVrows;
    bool noteVrowsDirty = true;

    // help dialog
    int helpScroll = 0;
    int helpPrevMode = M_BROWSE;

    // post-editor file copy
    std::string pendingOutlineTarget; // real path to copy editor output to
    std::string pendingJournalFile;   // temp journal filename used

    // confirm dialog
    std::string confirmMsg;
    int confirmAction = 0;  // 1=delete heading, 2=delete project, 3=clear file
    int confirmIdx = -1;    // subject index

    // tag management
    std::vector<std::string> tagList;
    int tagMgrSel = 0;
    std::string renameTargetTag;

    // long-press item menu (M_ITEM_MENU)
    int itemMenuSel = 0;

    // picker (for status/tags)
    struct PickerOpt { std::string value; std::string display; };
    std::vector<PickerOpt> pickerOpts;
    int pickerSel = 0;
    int pickerField = -1;
    std::set<int> pickerToggled;
} g;

// ── 当前编辑缓冲的 ImeField 形态 ────────────────────────────────────────
// 每个都是当场构造的两个指针（串 + 光标），不拷贝不接管，只为把 UTF-8 边界算术
// 收进 ui/ime_field.h 那一份；筛选框没有光标成员，是纯追加字段，所以 cursor 传 nullptr。
static ImeField olEditField()   { return ImeField{&g.editBuf, &g.editCur}; }
static ImeField olNoteField()   { return ImeField{&g.noteLines[g.noteRow], &g.noteCol}; }
static ImeField olFilterField() { return ImeField{&g.filterText, /*cursor=*/nullptr}; }

// ── 编辑态虚拟键盘 ────────────────────────────────────────────────────────
// 与写作/计划模式共用同一套键盘（editor_vk）：没连蓝牙键盘时自动弹出、连上自动
// 收起。列表浏览态不弹（下半屏留给大纲树），只有真要往输入框里打字时才占下半屏。
//
// 这些模式原来只有"输入法状态条"（drawIMEUI*），没有键盘——没接蓝牙键盘就完全打
// 不了字。现在键盘展开时由键盘自带候选条，状态条不再单画（见各处 draw 的尾段）。
static bool olVkEditing() {
    switch (g.mode) {
        // 整屏输入浮层（drawInputOverlay）：新建项目 / 增删改标题 / 编辑备注 / 标签
        case M_ADD_PROJECT: case M_ADD_HEADING: case M_ADD_SUB:
        case M_EDIT_NOTE:   case M_EDIT_NOTE_ML:
        case M_ADD_TAG:     case M_RENAME_TAG:
            return true;
        // 筛选是在树上直接打字
        case M_FILTER:
            return true;
        default:
            return false;
    }
}

// 键盘此刻确实占着屏幕下半部分吗（编辑态 + 已展开）。
static bool olVkActive() { return olVkEditing() && editorVkVisible(); }

// 输入态下半屏：键盘展开时画键盘面板（候选/编码行在面板顶部，所以原来那些 IME 条
// 不再单画），返回 true 表示已接管；否则按各界面原来的画法画 IME 条。
// fullscreen = true 走 drawIMEUIFullscreen，false 走 drawIMEUIWithStatusBar。
static bool drawInputPanel(bool fullscreen) {
    if (olVkActive()) {
        editorVkDraw();
        return true;
    }
    if (g_ime.composing()) {
        if (fullscreen) drawIMEUIFullscreen();
        else drawIMEUIWithStatusBar();
    }
    return false;
}

// ── Helpers ──────────────────────────────────────────────────────────────

static std::vector<std::string> listProjects() {
    std::vector<std::string> result;
    DIR *dir = opendir(OUTLINE_DIR);
    if (!dir) return result;
    struct dirent *entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_type == DT_DIR) {
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
            // check project.json exists
            std::string pj = std::string(OUTLINE_DIR) + "/" + entry->d_name + "/project.json";
            FILE *f = fopen(pj.c_str(), "rb");
            if (f) { fclose(f); result.push_back(entry->d_name); }
        }
    }
    closedir(dir);
    std::sort(result.begin(), result.end());
    return result;
}

static void loadOutline() {
    if (g.curProject < 0 || g.curProject >= (int)g.projects.size()) {
        g.nodes = nullptr;
        g.nodeCount = 0;
        return;
    }
    std::string path = std::string(OUTLINE_DIR) + "/" + g.projects[g.curProject] + "/project.json";
    g.outlineData = JsonValue::loadFromFile(path);
    if (g.outlineData.isNull() || !g.outlineData.has("nodes") || !g.outlineData["nodes"].isArray()) {
        g.outlineData = JsonValue::object();
        g.outlineData.set("nodes", JsonValue::array());
        g.nodes = nullptr;
        g.nodeCount = 0;
        return;
    }
    // Purge null-type nodes from old bug
    auto &nodes = g.outlineData["nodes"];
    int write = 0;
    for (int i = 0; i < (int)nodes.size(); i++) {
        if (!nodes[i].isNull())
            nodes.elements[write++] = nodes[i];
    }
    nodes.elements.resize(write);

    // Ensure bookmarks/tags before taking address of nodes.elements,
    // since any new root key inserted later (set or operator[] on a missing
    // key) reallocates memberValues and invalidates the pointer.
    if (!g.outlineData.has("bookmarks") || !g.outlineData["bookmarks"].isArray())
        g.outlineData.set("bookmarks", JsonValue::array());
    if (!g.outlineData.has("tags") || !g.outlineData["tags"].isArray())
        g.outlineData.set("tags", JsonValue::array());

    g.nodes = &g.outlineData["nodes"].elements;
    g.nodeCount = g.outlineData["nodes"].size();
}

static void buildTagList() {
    g.tagList.clear();
    // Collect from outlineData["tags"] — use has() to avoid mutating memberValues
    if (g.outlineData.has("tags")) {
        auto &tags = g.outlineData["tags"];
        if (tags.isArray()) {
            for (int i = 0; i < (int)tags.size(); i++) {
                std::string name = tags[i].asString();
                if (name.empty()) continue;
                bool dup = false;
                for (auto &t : g.tagList) if (t == name) { dup = true; break; }
                if (!dup) g.tagList.push_back(name);
            }
        }
    }
    // Collect from node tags
    if (g.nodes) {
        for (size_t i = 0; i < g.nodeCount; i++) {
            auto &node = (*g.nodes)[i];
            if (!node.has("tags")) continue;
            auto &tt = node["tags"];
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

static void openOutlinePicker(int fieldIdx) {
    g.pickerOpts.clear();
    g.pickerField = fieldIdx;
    auto &node = (*g.nodes)[g.detailNodeIdx];

    if (fieldIdx == 1) {  // status
        for (int i = 0; i < OUTLINE_STATUS_COUNT; i++)
            g.pickerOpts.push_back({OUTLINE_STATUS[i], OUTLINE_STATUS_DISPLAY[i]});
        std::string cur = node["status"].asString("draft");
        g.pickerSel = 0;
        for (int i = 0; i < OUTLINE_STATUS_COUNT; i++)
            if (OUTLINE_STATUS[i] == cur) { g.pickerSel = i; break; }
    } else if (fieldIdx == 4) {  // tags
        buildTagList();
        for (auto &t : g.tagList)
            g.pickerOpts.push_back({t, "#" + t});
        g.pickerSel = 0;
        g.pickerToggled.clear();
        auto &tt = node["tags"];
        if (tt.isArray()) {
            for (int i = 0; i < (int)tt.size(); i++) {
                for (int j = 0; j < (int)g.pickerOpts.size(); j++) {
                    if (g.pickerOpts[j].value == tt[i].asString()) {
                        g.pickerToggled.insert(j); break;
                    }
                }
            }
        }
    }
    g.mode = M_PICKER;
}

static void saveOutline() {
    if (g.curProject < 0 || g.curProject >= (int)g.projects.size()) return;
    std::string path = std::string(OUTLINE_DIR) + "/" + g.projects[g.curProject] + "/project.json";
    if (!JsonValue::saveToFile(path, g.outlineData)) {
        mkdir(OUTLINE_DIR, 0777);
        mkdir((std::string(OUTLINE_DIR) + "/" + g.projects[g.curProject]).c_str(), 0777);
        JsonValue::saveToFile(path, g.outlineData);
    }
}

static std::string makeId() {
    time_t now; time(&now); struct tm *tm = localtime(&now);
    char buf[32];
    static int seq = 0;
    snprintf(buf, sizeof(buf), "n%02d%02d%02d_%d",
             tm->tm_hour, tm->tm_min, tm->tm_sec, seq++);
    return buf;
}

// Convert heading title to a safe filename
static std::string safeFilename(const std::string &title) {
    std::string out;
    for (char c : title) {
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|')
            out += '_';
        else
            out += c;
    }
    if (out.empty()) out = "untitled";
    if (out.size() > 40) {
        // 按字节切会劈开一个多字节字符：退到字符边界再切。切出非法 UTF-8 不只是
        // 文件名难看 —— 它被写进 node["file"]，还会让两个前 40 字节相同的标题
        // 共用同一个 .txt 正文文件，互相覆盖。
        size_t cut = 40;
        while (cut > 0 && ((unsigned char)out[cut] & 0xC0) == 0x80) cut--;   // 退过续字节
        out.resize(cut);
    }
    return out + ".txt";
}

// Ensure the outline content file exists
static std::string ensureContentFile(const std::string &project, const std::string &filename) {
    std::string dir = std::string(OUTLINE_DIR) + "/" + project;
    mkdir(OUTLINE_DIR, 0777);
    mkdir(dir.c_str(), 0777);
    std::string path = dir + "/" + filename;
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) {
        f = fopen(path.c_str(), "w");
        if (f) fclose(f);
    } else {
        fclose(f);
    }
    return path;
}

// Read content file, strip journal header if present
static std::string readContentFile(const std::string &path) {
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) return "";
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return ""; }
    if (sz > MAX_OUTLINE_CONTENT_FILE_SIZE) { fclose(f); return ""; }
    std::string content(static_cast<size_t>(sz), '\0');
    if (fread(&content[0], 1, static_cast<size_t>(sz), f) != static_cast<size_t>(sz)) {
        fclose(f); return "";
    }
    fclose(f);
    return content;
}

// ── Filter helpers ───────────────────────────────────────────────────────
static std::vector<int> g_filteredIdx;

static void rebuildFilter() {
    g_filteredIdx.clear();
    if (!g.nodes) return;
    // Build set of nodes hidden by folding
    std::set<int> hiddenByFold;
    for (int fi : g.foldedNodes) {
        if (fi < 0 || (size_t)fi >= g.nodeCount) continue;
        int foldLvl = (*g.nodes)[fi]["level"].asInt(0);
        for (int j = fi + 1; j < (int)g.nodeCount; j++) {
            if ((*g.nodes)[j]["level"].asInt(0) <= foldLvl) break;
            hiddenByFold.insert(j);
        }
    }
    for (size_t i = 0; i < g.nodeCount; i++) {
        if (hiddenByFold.count((int)i)) continue;
        // Tag filter
        if (!g.filterTags.empty()) {
            auto &tt = (*g.nodes)[i]["tags"];
            bool tagMatch = false;
            if (tt.isArray()) {
                for (int j = 0; j < (int)tt.size() && !tagMatch; j++)
                    for (auto &ft : g.filterTags)
                        if (tt[j].asString() == ft) { tagMatch = true; break; }
            }
            if (!tagMatch) continue;
        }
        if (g.filterText.empty()) {
            g_filteredIdx.push_back((int)i);
        } else {
            std::string title = (*g.nodes)[i]["title"].asString();
            if (title.find(g.filterText) != std::string::npos)
                g_filteredIdx.push_back((int)i);
        }
    }
    if (g.sel >= (int)g_filteredIdx.size()) g.sel = (int)g_filteredIdx.size() - 1;
    if (g.sel < 0) g.sel = 0;
}

// ── Markdown export ─────────────────────────────────────────────────────
static std::string exportMD() {
    std::string md;
    if (g.curProject < 0 || g.curProject >= (int)g.projects.size())
        return "# 大纲\n";
    md = "# " + g.projects[g.curProject] + "\n\n";
    if (!g.nodes) return md;
    for (size_t i = 0; i < g.nodeCount; i++) {
        auto &node = (*g.nodes)[i];
        int lvl = node["level"].asInt(0);
        std::string title = node["title"].asString();
        std::string file = node["file"].asString();

        // heading markers
        std::string prefix;
        for (int j = 0; j <= lvl && j < 6; j++) prefix += "#";
        md += prefix + " " + title + "\n";

        // read and append content
        if (!file.empty() && g.curProject >= 0) {
            std::string fpath = std::string(OUTLINE_DIR) + "/" + g.projects[g.curProject] + "/" + file;
            std::string content = readContentFile(fpath);
            if (!content.empty()) {
                md += "\n" + content + "\n\n";
            }
        }
    }
    return md;
}

// ── Tree prefix helpers ──────────────────────────────────────────────────
static bool isLastNodeAtLevel(int idx, int lvl) {
    if (!g.nodes) return true;
    for (int j = idx + 1; j < (int)g.nodeCount; j++) {
        int jlvl = (*g.nodes)[j]["level"].asInt(0);
        if (jlvl == lvl) return false;
        if (jlvl < lvl) break;
    }
    return true;
}

static std::string nodeTreePrefix(int idx) {
    if (!g.nodes || idx < 0 || idx >= (int)g.nodeCount) return "";
    int lvl = (*g.nodes)[idx]["level"].asInt(0);
    std::string prefix;
    for (int a = 0; a < lvl; a++) {
        int ancIdx = -1;
        for (int j = idx - 1; j >= 0; j--) {
            int jlvl = (*g.nodes)[j]["level"].asInt(0);
            if (jlvl == a) { ancIdx = j; break; }
            if (jlvl < a) break;
        }
        if (ancIdx >= 0 && !isLastNodeAtLevel(ancIdx, a))
            prefix += "│ ";
        else
            prefix += "  ";
    }
    if (lvl > 0)
        prefix += isLastNodeAtLevel(idx, lvl) ? "└─ " : "├─ ";
    else
        prefix = "◆ ";
    return prefix;
}

// ── Drawing ──────────────────────────────────────────────────────────────

static void drawIMEStatus() {
    drawIMEUIWithStatusBar();
}

static void drawInputOverlay(const char *title) {
    ui_clear();
    ui_draw_text_centered(ui_title_baseline(), title, false, true);
    u8g2_DrawHLine(g_u8g2, 0, ui_title_baseline() + g_font.descent() + 4, SCREEN_W);
    std::string display = g.editBuf.empty() ? " " : g.editBuf;
    int ty = ui_title_baseline() + g_font.descent() + 12 + g_font.ascent();
    ui_draw_text(4, ty, display.c_str());
    int cx = g_font.textWidth(g.editBuf.substr(0, g.editCur).c_str());
    u8g2_SetDrawColor(g_u8g2, 0);
    u8g2_DrawBox(g_u8g2, 4 + cx, ty + 4, 8, 3);
    u8g2_SetDrawColor(g_u8g2, 1);

    // 键盘展开时：面板自带候选条 + 状态栏右侧键盘开关；蓝牙键盘态沿用原来的 IME 条。
    if (drawInputPanel(true)) {
        ui_draw_status("Enter确定 Esc取消", "");
        editorVkDrawIcon();
    } else if (!g_ime.composing()) {
        ui_draw_status("Enter确定 Esc取消", imeStatusLabel(g.imeActive).c_str());
    }
    u8g2_SetDrawColor(g_u8g2, 0);
    ui_commit();
}

static void drawProjectList();
static void drawOutline();
static void drawBookmarkMgr();
static void drawOutlineDetail();
static void drawHelp();

static const char *HELP_LINES[] = {
    "── 项目列表 ──",
    "↑↓    选择",
    "Enter 打开项目",
    "n     新建项目",
    "d     删除项目",
    "q/Esc 返回",
    "",
    "── 大纲列表 ──",
    "↑↓    选择",
    "a     添加标题",
    "i     添加子标题",
    "r     重命名",
    "Enter 详情面板",
    "f     关联文件",
    "s     摘要",
    "d     删除标题",
    "Tab   切换项目",
    "/     筛选",
    "t     标签管理",
    "j/k   上/下移任务",
    "h/l   提/降层级",
    "q/Esc 返回项目",
    "",
    "── 详情面板 ──",
    "↑↓    选择字段",
    "Enter 编辑字段",
    "f     关联文件",
    "s     摘要",
    "Esc   返回",
    "",
    "── 通用 ──",
    "?     显示帮助",
};
static const int HELP_LINE_COUNT = sizeof(HELP_LINES) / sizeof(HELP_LINES[0]);

static void drawHelp() {
    if (g.mode == M_HELP && g.helpPrevMode == M_PROJECTS) drawProjectList();
    else if (g.mode == M_HELP && g.helpPrevMode == M_DETAIL) drawOutlineDetail();
    else drawOutline();

    int boxW = 300, boxH = 250;
    int boxX = (SCREEN_W - boxW) / 2;
    int boxY = (SCREEN_H - boxH) / 2;
    u8g2_SetDrawColor(g_u8g2, 1);
    u8g2_DrawBox(g_u8g2, boxX, boxY, boxW, boxH);
    u8g2_SetDrawColor(g_u8g2, 0);
    u8g2_DrawFrame(g_u8g2, boxX, boxY, boxW, boxH);

    int titleY = boxY + 8 + g_font.ascent();
    g_font.drawText(boxX + (boxW - g_font.textWidth("快捷键帮助")) / 2, titleY, "快捷键帮助", false);
    u8g2_DrawHLine(g_u8g2, boxX + 4, titleY + g_font.descent() + 4, boxW - 8);

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
        bool isHeader = ((unsigned char)line[0] == 0xE2);
        ui_draw_text(boxX + 12, ly + g_font.ascent(), line, false, isHeader);
    }

    ui_draw_status("↑↓滚动 Esc返回", "");
    u8g2_SetDrawColor(g_u8g2, 0);
    ui_commit();
}

static void drawOutlineDetailInner() {
    ui_clear();
    if (!g.nodes || g.detailNodeIdx < 0 || g.detailNodeIdx >= (int)g.nodeCount) return;
    auto &node = (*g.nodes)[g.detailNodeIdx];
    std::string title = node["title"].asString();
    int y = g_font.ascent();
    ui_draw_text(4, y, title.c_str(), false, true);
    u8g2_DrawHLine(g_u8g2, 0, FONT_H + 4, SCREEN_W);
    y = FONT_H + 8 + LINE_SPACING;

    struct Field { const char *label; const char *key; char type; };
    static const Field fields[] = {
        {"标题",   "title",    's'},
        {"状态",   "status",   't'},
        {"关键词", "keywords", 's'},
        {"备注",   "note",     'm'},
        {"标签",   "tags",     'g'},
    };
    static const int NF = 5;

    for (int i = 0; i < NF; i++) {
        bool sel = (i == g.detailField);
        auto &f = fields[i];
        std::string val;
        if (f.type == 'm') {
            val = node[f.key].asString();
            if (val.empty()) val = "(空)";
            else {
                size_t nl = val.find('\n');
                if (nl != std::string::npos) val = val.substr(0, nl) + "…";
            }
        } else if (f.type == 't') {
            std::string sv = node["status"].asString("draft");
            val = "(未设)";
            for (int k = 0; k < OUTLINE_STATUS_COUNT; k++)
                if (OUTLINE_STATUS[k] == sv) { val = OUTLINE_STATUS_DISPLAY[k]; break; }
        } else if (f.type == 'g') {
            auto &tt = node["tags"];
            if (tt.isArray() && tt.size() > 0) {
                val = "#" + tt[0].asString();
                for (int j = 1; j < (int)tt.size(); j++) val += " #" + tt[j].asString();
            } else val = "(无)";
        } else {
            val = node[f.key].asString();
            if (val.empty()) val = "(空)";
        }
        char buf[80];
        snprintf(buf, sizeof(buf), "%s: %s", f.label, val.c_str());
        ui_draw_text(8, y + i * LINE_SPACING, buf, sel);
    }

    // Show associated file
    std::string file = node["file"].asString();
    if (!file.empty()) {
        char fbuf[80];
        snprintf(fbuf, sizeof(fbuf), "文件: %s", file.c_str());
        ui_draw_text(8, y + NF * LINE_SPACING, fbuf, false);
    }

    ui_draw_status("Enter编辑 ↑↓选择 f:关联 Esc返回", "");
    drawIMEStatus();
}

static void drawOutlineDetail() {
    drawOutlineDetailInner();
    ui_commit();
}

static void drawSummary() {
    int boxX = (SCREEN_W - 300) / 2;
    int boxY = (SCREEN_H - 250) / 2;
    int boxW = 300, boxH = 250;
    u8g2_SetDrawColor(g_u8g2, 1);
    u8g2_DrawBox(g_u8g2, boxX, boxY, boxW, boxH);
    u8g2_SetDrawColor(g_u8g2, 0);
    u8g2_DrawFrame(g_u8g2, boxX, boxY, boxW, boxH);
    ui_draw_text_centered(boxY + FONT_H, "摘要", false, true);

    if (!g.nodes || g.summaryNodeIdx < 0 || g.summaryNodeIdx >= (int)g.nodeCount) return;
    auto &node = (*g.nodes)[g.summaryNodeIdx];
    int textX = boxX + 8;
    int y = boxY + FONT_H + 8;
    int contentW = boxW - 16;

    // Separator under title
    u8g2_DrawHLine(g_u8g2, boxX + 4, y, boxW - 8);
    y += 8 + 15;

    // Status
    std::string st = node["status"].asString("draft");
    const char *stDisp = "草稿";
    for (int s = 0; s < OUTLINE_STATUS_COUNT; s++)
        if (OUTLINE_STATUS[s] == st) { stDisp = OUTLINE_STATUS_DISPLAY[s]; break; }
    char line[128];
    snprintf(line, sizeof(line), "状态: %s", stDisp);
    ui_draw_text(textX, y, line, false);
    y += LINE_SPACING;

    // Keywords
    std::string kw = node["keywords"].asString();
    snprintf(line, sizeof(line), "关键词: %s", kw.empty() ? "(无)" : kw.c_str());
    ui_draw_text(textX, y, line, false);
    y += LINE_SPACING;

    // Tags
    auto &tt = node["tags"];
    std::string tagStr;
    if (tt.isArray() && tt.size() > 0) {
        for (int j = 0; j < (int)tt.size(); j++) {
            if (j > 0) tagStr += " ";
            tagStr += "#" + tt[j].asString();
        }
    }
    snprintf(line, sizeof(line), "标签: %s", tagStr.empty() ? "(无)" : tagStr.c_str());
    ui_draw_text(textX, y, line, false);
    y += LINE_SPACING;

    // Notes - inline word-wrap, continues on same line after "备注:"
    std::string note = node["note"].asString();
    if (note.empty()) {
        ui_draw_text(textX, y, "备注: (无)", false);
    } else {
        // Flatten note into a single line (replace newlines with spaces)
        std::string flatNote;
        for (char c : note) { flatNote += (c == '\n') ? ' ' : c; }
        std::string prefix = "备注: ";
        // First line starts after "备注:" prefix
        std::string firstLine = prefix + flatNote;
        // Word-wrap the combined text within contentW
        std::vector<std::string> wrapped;
        int pos = 0;
        int len = (int)firstLine.length();
        while (pos < len) {
            int end = pos;
            int lastBreak = -1;
            while (end < len) {
                std::string sub = firstLine.substr(pos, end - pos + 1);
                if (g_font.textWidth(sub.c_str()) > contentW) break;
                if (firstLine[end] == ' ') lastBreak = end + 1;
                end++;
            }
            if (end >= len) { wrapped.push_back(firstLine.substr(pos)); break; }
            if (lastBreak > pos) {
                wrapped.push_back(firstLine.substr(pos, lastBreak - pos));
                pos = lastBreak;
                while (pos < len && firstLine[pos] == ' ') pos++;
            } else if (end > pos) {
                wrapped.push_back(firstLine.substr(pos, end - pos));
                pos = end;
            } else {
                wrapped.push_back(firstLine.substr(pos, 1));
                pos++;
            }
        }
        int textAreaH = boxY + boxH - 16 - y;
        int maxVis = textAreaH / LINE_SPACING;
        if (maxVis < 1) maxVis = 1;
        if (g.summaryScroll > (int)wrapped.size() - maxVis) g.summaryScroll = (int)wrapped.size() - maxVis;
        if (g.summaryScroll < 0) g.summaryScroll = 0;
        for (int i = 0; i < maxVis && (g.summaryScroll + i) < (int)wrapped.size(); i++)
            g_font.drawText(textX, y + i * LINE_SPACING, wrapped[g.summaryScroll + i].c_str(), false);
    }

    ui_draw_status("\xe2\x86\x91\xe2\x86\x93\xe6\xbb\x9a\xe5\x8a\xa8 Esc\xe8\xbf\x94\xe5\x9b\x9e", "");
    u8g2_SetDrawColor(g_u8g2, 0);
    ui_commit();
}

static void drawBookmarkMgr() {
    ui_clear();
    ui_draw_text(4, g_font.ascent(), "书签管理", false, true);
    u8g2_DrawHLine(g_u8g2, 0, FONT_H + 4, SCREEN_W);
    int y = FONT_H + 8 + LINE_SPACING;
    auto &bmArr = g.outlineData["bookmarks"];
    int bmCount = bmArr.isArray() ? (int)bmArr.size() : 0;
    int vis = (STATUS_Y - y + LINE_SPACING - 1) / LINE_SPACING;
    if (vis < 1) vis = 1;
    if (g.bmMgrSel < g.scroll) g.scroll = g.bmMgrSel;
    if (g.bmMgrSel >= g.scroll + vis) g.scroll = g.bmMgrSel - vis + 1;
    for (int i = 0; i < vis && (g.scroll + i) < bmCount; i++) {
        int bi = g.scroll + i;
        auto &bm = bmArr[bi];
        std::string bmTitle = bm["title"].asString();
        bool sel = (bi == g.bmMgrSel);
        ui_draw_text(8, y + i * LINE_SPACING, bmTitle.c_str(), sel);
    }
    if (bmCount == 0) ui_draw_text(8, y, "暂无书签 — 在大纲中按m添加");
    char sl[96];
    snprintf(sl, sizeof(sl), "Enter:跳转 d:删除 Esc:返回 %d项", bmCount);
    ui_draw_status(sl, "");
    drawIMEStatus(); ui_commit();
}

// ── 悬浮「+」新建键 ───────────────────────────────────────────────────────
// 贴在状态栏上方右侧。列表下边界主动让出一条带，所以它盖不到正文，看上去是浮在
// 空白上。项目列表层 = 新建项目(喂 'n')，进了某个项目里 = 新建标题(喂 'a')——
// 都只是把点按翻译成既有的普通键码，不另写一套新建逻辑。
static int outlineFabSize() { return FONT_H + 6; }
static int outlineFabX() { return SCREEN_W - outlineFabSize() - 12; }
static int outlineFabY() { return STATUS_BAR_Y - outlineFabSize() - 8; }
static int outlineListMaxY();   // 定义见方向键那一块之后（要引用悬浮键的几何）

static void drawOutlineFab() {
    int s = outlineFabSize();
    int x = outlineFabX(), y = outlineFabY();
    int cx = x + s / 2, cy = y + s / 2, arm = s / 3;
    if (arm < 4) arm = 4;
    u8g2_SetDrawColor(g_u8g2, 0);
    u8g2_DrawBox(g_u8g2, x, y, s, s);
    u8g2_SetDrawColor(g_u8g2, 1);
    u8g2_DrawBox(g_u8g2, cx - 2, cy - arm / 2, 5, arm + (arm & 1));      // 竖笔
    u8g2_DrawBox(g_u8g2, cx - arm / 2, cy - 2, arm + (arm & 1), 5);      // 横笔
    u8g2_SetDrawColor(g_u8g2, 0);
}

static bool outlineFabHit(int x, int y) {
    int s = outlineFabSize();
    return x >= outlineFabX() && x < outlineFabX() + s && y >= outlineFabY() && y < outlineFabY() + s;
}

// ── 方向键 + 「+子标题」悬浮键（只在项目详情树里出现，与计划模式同款）─────────
// 契约与计划模式一致：命中就喂一个既有的普通键码，主处理里那条重排/升降级逻辑
// 照旧执行，这里不另写一套。大纲树里 'h'=升级 'l'=降级 'j'=上移 'k'=下移
// 'i'=在选中条目下挂子标题 —— 语义与 GTD 项目详情完全相同。
#define OLPAD_BTN (FONT_H + 10)
#define OLPAD_GAP 6
// 只在树浏览态且没在筛选（筛选态下半屏是候选条）时露出这排悬浮键。
static bool outlineDirPadShown() { return g.mode == M_BROWSE && g.filterText.empty(); }
static int outlineDirPadY() { return STATUS_BAR_Y - 8 - OLPAD_BTN; }
static int outlineDirPadX() { return 4; }
static const int kOutlinePadArrow[4] = {2, 3, 0, 1};   // 2=← 3=→ 0=↑ 1=↓
static const int kOutlinePadKey[4]   = {'h', 'l', 'j', 'k'};

static int outlineSubFabX() { return outlineFabX() - outlineFabSize() - 8; }
static int outlineSubFabY() { return outlineFabY(); }

static bool outlineSubFabHit(int x, int y) {
    if (!outlineDirPadShown()) return false;
    int s = outlineFabSize();
    return x >= outlineSubFabX() && x < outlineSubFabX() + s &&
           y >= outlineSubFabY() && y < outlineSubFabY() + s;
}

// 空心方框 + 实心「+」，与「+」实心键一眼分得开。
static void drawOutlineSubFab() {
    if (!outlineDirPadShown()) return;
    int s = outlineFabSize();
    int x = outlineSubFabX(), y = outlineSubFabY();
    int cx = x + s / 2, cy = y + s / 2, arm = s / 3;
    if (arm < 4) arm = 4;
    const int t = 2;
    u8g2_SetDrawColor(g_u8g2, 1);
    u8g2_DrawBox(g_u8g2, x, y, s, s);
    u8g2_SetDrawColor(g_u8g2, 0);
    u8g2_DrawBox(g_u8g2, x, y, s, t);
    u8g2_DrawBox(g_u8g2, x, y + s - t, s, t);
    u8g2_DrawBox(g_u8g2, x, y, t, s);
    u8g2_DrawBox(g_u8g2, x + s - t, y, t, s);
    u8g2_DrawBox(g_u8g2, cx - 2, cy - arm / 2, 5, arm + (arm & 1));
    u8g2_DrawBox(g_u8g2, cx - arm / 2, cy - 2, arm + (arm & 1), 5);
    u8g2_SetDrawColor(g_u8g2, 0);
}

static int outlineDirPadHit(int x, int y) {
    if (!outlineDirPadShown()) return -1;
    int y0 = outlineDirPadY();
    if (y < y0 || y >= y0 + OLPAD_BTN) return -1;
    int x0 = outlineDirPadX();
    if (x < x0) return -1;
    int i = (x - x0) / (OLPAD_BTN + OLPAD_GAP);
    if (i < 0 || i > 3) return -1;
    int bx = x0 + i * (OLPAD_BTN + OLPAD_GAP);
    if (x >= bx + OLPAD_BTN) return -1;   // 落在两个键之间的缝里
    return i;
}

// 白色实心箭头：逐行加宽的横条堆三角头 + 细杆。dir: 0=上 1=下 2=左 3=右。
static void outlineDrawArrow(int cx, int cy, int size, int dir) {
    const int half = size / 4;
    const int head = size / 2;
    const int shaft = (size / 4) | 1;
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

static void drawOutlineDirPad() {
    if (!outlineDirPadShown()) return;
    int s = OLPAD_BTN;
    int y = outlineDirPadY();
    int asz = s - 14;
    if (asz < 10) asz = 10;
    if (asz > s - 8) asz = s - 8;
    for (int i = 0; i < 4; i++) {
        int x = outlineDirPadX() + i * (s + OLPAD_GAP);
        u8g2_SetDrawColor(g_u8g2, 0);
        u8g2_DrawBox(g_u8g2, x, y, s, s);
        u8g2_SetDrawColor(g_u8g2, 1);
        outlineDrawArrow(x + s / 2, y + s / 2, asz, kOutlinePadArrow[i]);
        u8g2_SetDrawColor(g_u8g2, 0);
    }
}

// 列表可用下边界（给悬浮键让位）。方向键那排更靠左也更高，占了就要再抬一点。
static int outlineListMaxY() {
    if (olVkActive()) return editorVkTop() - 4;   // 筛选时下半屏是键盘
    if (outlineDirPadShown()) return outlineDirPadY() - 4;
    return outlineFabY() - 4;
}

static void drawProjectList() {
    ui_clear();
    ui_draw_text(4, g_font.ascent(), "选择项目", false, true);
    u8g2_DrawHLine(g_u8g2, 0, FONT_H + 4, SCREEN_W);

    int y = FONT_H + 8 + LINE_SPACING + 2;
    int vis = (outlineListMaxY() - y + LINE_SPACING - 1) / LINE_SPACING;
    if (vis < 1) vis = 1;

    if (g.sel < g.scroll) g.scroll = g.sel;
    if (g.sel >= g.scroll + vis) g.scroll = g.sel - vis + 1;

    for (int i = 0; i < vis && (g.scroll + i) < (int)g.projects.size(); i++) {
        bool sel = (g.scroll + i == g.sel);
        ui_draw_text(8, y + i * LINE_SPACING, g.projects[g.scroll + i].c_str(), sel);
    }
    ui_draw_status("n:新建 Enter:打开 d:删除", "");
    drawOutlineFab();
}

static void drawOutline() {
    ui_clear();

    // project title
    std::string title;
    if (g.curProject >= 0 && g.curProject < (int)g.projects.size())
        title = g.projects[g.curProject];
    else
        title = "大纲";
    ui_draw_text(4, g_font.ascent(), title.c_str(), false, true);
    u8g2_DrawHLine(g_u8g2, 0, FONT_H + 4, SCREEN_W);

    int y = FONT_H + 8 + LINE_SPACING;
    bool composingFilter = (g.mode == M_FILTER && g_ime.composing());
    int maxY = olVkActive() ? (editorVkTop() - 4)
                            : (composingFilter ? imeStatusPanelTopY() : outlineListMaxY());
    int vis = (maxY - y + LINE_SPACING - 1) / LINE_SPACING;
    if (vis < 1) vis = 1;

    if (g.sel < g.scroll) g.scroll = g.sel;
    if (g.sel >= g.scroll + vis) g.scroll = g.sel - vis + 1;

    auto &filtered = g.filterText.empty() ? g_filteredIdx : g_filteredIdx;
    if (filtered.empty() && g.filterText.empty()) {
        // show all nodes
        if (!g.nodes) {
            ui_draw_text(8, y, "空项目 — 按a添加标题");
            ui_draw_status("a:标题 i:子标题 n:项目 Tab:切换", "");
            return;
        }
        filtered.clear();
        for (size_t i = 0; i < g.nodeCount; i++) filtered.push_back((int)i);
        g_filteredIdx = filtered;
    }

    for (int i = 0; i < vis && (g.scroll + i) < (int)filtered.size(); i++) {
        int ni = filtered[g.scroll + i];
        auto &node = (*g.nodes)[ni];
        bool sel = (g.scroll + i == g.sel);
        int lvl = node["level"].asInt(0);
        std::string title = node["title"].asString();

        char buf[96];
        std::string tprefix = g.filterText.empty() ? nodeTreePrefix(ni) : std::string(lvl * 2, ' ');
        // Check if this node has children (for fold indicator)
        bool hasChildren = false;
        if (ni + 1 < (int)g.nodeCount && (*g.nodes)[ni + 1]["level"].asInt(0) > lvl)
            hasChildren = true;
        bool isFolded = g.foldedNodes.count(ni) > 0;
        std::string foldMark;
        if (hasChildren && isFolded) foldMark = "▸ ";
        else if (hasChildren) foldMark = "▾ ";
        snprintf(buf, sizeof(buf), "%s%s%s", tprefix.c_str(), foldMark.c_str(), title.c_str());
        ui_draw_text(8, y + i * LINE_SPACING, buf, sel);

        // Bookmark indicator
        auto &bmArr = g.outlineData["bookmarks"];
        bool isBookmarked = false;
        std::string nodeId = node["id"].asString();
        if (bmArr.isArray()) {
            for (int bi = 0; bi < (int)bmArr.size(); bi++)
                if (bmArr[bi]["id"].asString() == nodeId) { isBookmarked = true; break; }
        }

        // show indicators: ★ status [M] before file icon
        int rightX = SCREEN_W - 4;
        std::string file = node["file"].asString();
        if (!file.empty()) rightX -= FILE_ICON_W;
        if (isBookmarked) {
            int bw = g_font.textWidth("★") + 2;
            rightX -= bw;
            g_font.drawText(rightX, y + i * LINE_SPACING, "★", false);
        }
        // Status symbol (replaces [K])
        std::string status = node["status"].asString("draft");
        const char *statusSym = "○";  // draft default
        for (int s = 0; s < OUTLINE_STATUS_COUNT; s++) {
            if (OUTLINE_STATUS[s] == status) {
                statusSym = (const char *[]){"○", "◐", "●", "✎"}[s];
                break;
            }
        }
        {
            int sw = g_font.textWidth(statusSym) + 2;
            rightX -= sw;
            g_font.drawText(rightX, y + i * LINE_SPACING, statusSym, false);
        }
        std::string note = node["note"].asString();
        if (!note.empty()) {
            int mw = g_font.textWidth("[M]") + 2;
            rightX -= mw;
            g_font.drawText(rightX, y + i * LINE_SPACING, "[M]", false);
        }
        // show file indicator
        if (!file.empty()) {
            drawFileIcon(SCREEN_W - FILE_ICON_W - 4, y + i * LINE_SPACING);
        }
    }

    if (!g.filterText.empty() && !composingFilter && !olVkActive()) {
        // 键盘展开时这条浮标会画在键盘最下排上，改由下面的状态栏提示承载。
        char fb[64];
        snprintf(fb, sizeof(fb), "筛选: %s", g.filterText.c_str());
        ui_draw_text(4, STATUS_Y - LINE_SPACING + 2, fb, true);
    }
    if (!g.filterTags.empty() && g.filterText.empty()) {
        char fb[64];
        int fn = snprintf(fb, sizeof(fb), "标签:");
        for (auto &ft : g.filterTags) {
            // snprintf 返回的是"本该写入的长度"，拿它当已写长度去续写，一旦超界
            // `sizeof(fb) - fn` 就下溢成巨大的 size_t，下一句直接往栈上写。装不下就停。
            if (fn >= (int)sizeof(fb) - 1) break;
            fn += snprintf(fb + fn, sizeof(fb) - fn, " #%s", ft.c_str());
        }
        ui_draw_text(4, STATUS_Y - LINE_SPACING + 2, fb, true);
    }

    // Status bar: ?:帮助 | keywords #tags
    {
        char sl[128];
        int n;
        if (olVkActive() && !g.filterText.empty())
            n = snprintf(sl, sizeof(sl), "筛选:%s", g.filterText.c_str());
        else
            n = snprintf(sl, sizeof(sl), "?:帮助");
        if (g.nodes && g.nodeCount > 0) {
            int idx = g.filterText.empty() ? g.sel : (g.sel < (int)g_filteredIdx.size() ? g_filteredIdx[g.sel] : -1);
            if (idx >= 0 && (size_t)idx < g.nodeCount) {
                auto &node = (*g.nodes)[idx];
                std::string kw = node["keywords"].asString();
                // 同上：每次续写前先夹住，n 可能已被上一条的"本该长度"顶到 cap 之外。
                if (!kw.empty() && n < (int)sizeof(sl) - 1)
                    n += snprintf(sl + n, sizeof(sl) - n, " | %s", kw.c_str());
                auto &tt = node["tags"];
                if (tt.isArray() && tt.size() > 0) {
                    if (kw.empty() && n < (int)sizeof(sl) - 1)
                        n += snprintf(sl + n, sizeof(sl) - n, " |");
                    for (int j = 0; j < (int)tt.size(); j++) {
                        if (n >= (int)sizeof(sl) - 1) break;
                        n += snprintf(sl + n, sizeof(sl) - n, " #%s", tt[j].asString().c_str());
                    }
                }
            }
        }
        ui_draw_status(sl, (g.mode == M_FILTER && !olVkActive())
                               ? imeStatusLabel(g.imeActive).c_str() : "");
    }

    // 筛选态下半屏归输入法/键盘：展开键盘时画面板 + 状态栏右侧开关图标，
    // 否则沿用原来的 IME 候选条。键盘开关图标必须在状态栏之后画。
    if (olVkActive()) { drawInputPanel(false); editorVkDrawIcon(); }
    else if (composingFilter) drawIMEStatus();
    // 只在真正的树浏览态画悬浮键：筛选态下半屏是输入法候选条，这里再浮一个
    // 实心方块会跟候选条打架，而筛选时也用不到"新建"。
    if (g.mode == M_BROWSE) { drawOutlineSubFab(); drawOutlineFab(); drawOutlineDirPad(); }
}

// ── 长按列表项：编辑菜单（与计划模式同款）────────────────────────────────
// 触屏没有键盘的 r/d/h/l/i，长按标题行就弹一个小浮层。菜单只负责"把点按命中翻译成
// 既有的普通键码"，动作实现全在下面对应的键盘分支里，不会跑偏。
enum OlMenuAct { OMA_RENAME, OMA_SUBTASK, OMA_PROMOTE, OMA_DEMOTE, OMA_DELETE, OMA_CANCEL };

static const OlMenuAct kOlMenuActs[] = { OMA_RENAME, OMA_SUBTASK, OMA_PROMOTE, OMA_DEMOTE,
                                         OMA_DELETE, OMA_CANCEL };
static const int kOlMenuCount = 6;

static const char *outlineItemMenuLabel(OlMenuAct a) {
    switch (a) {
        case OMA_RENAME:  return "重命名";
        case OMA_SUBTASK: return "添加子标题";
        case OMA_PROMOTE: return "提升层级";
        case OMA_DEMOTE:  return "降低层级";
        case OMA_DELETE:  return "删除";
        default:          return "取消";
    }
}

static void outlineItemMenuBoxRect(int *bx, int *by, int *bw, int *bh) {
    int w = 0;
    for (int i = 0; i < kOlMenuCount; i++) {
        int tw = g_font.textWidth(outlineItemMenuLabel(kOlMenuActs[i])) + 2 * FONT_H;
        if (tw > w) w = tw;
    }
    if (w < 160) w = 160;
    if (w > SCREEN_W - 32) w = SCREEN_W - 32;
    *bw = w;
    *bh = kOlMenuCount * LINE_SPACING + 16;
    *bx = (SCREEN_W - w) / 2;
    *by = (SCREEN_H - *bh) / 2;
}

static int outlineItemMenuRowY(int by, int i) { return by + 8 + g_font.ascent() + i * LINE_SPACING; }

static void drawOutlineItemMenu() {
    drawOutline();   // 底下的树照画，浮层盖在上面

    int bx, by, bw, bh;
    outlineItemMenuBoxRect(&bx, &by, &bw, &bh);

    u8g2_SetDrawColor(g_u8g2, 1);
    u8g2_DrawBox(g_u8g2, bx, by, bw, bh);
    u8g2_SetDrawColor(g_u8g2, 0);
    u8g2_DrawFrame(g_u8g2, bx, by, bw, bh);

    for (int i = 0; i < kOlMenuCount; i++) {
        const char *lb = outlineItemMenuLabel(kOlMenuActs[i]);
        int y = outlineItemMenuRowY(by, i);
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

// 当前可见行数（与 drawOutline 里那段同式；长按命中要用它挡掉滚出屏的行）。
static int outlineListRows() {
    int y0 = FONT_H + 8 + LINE_SPACING;
    int vis = (outlineListMaxY() - y0 + LINE_SPACING - 1) / LINE_SPACING;
    return vis < 1 ? 1 : vis;
}

// ── 翻页步长（触摸上下滑 = 整页翻）──────────────────────────────────────
// "从 y0 起到 maxY 装得下几行"，与对应的绘制函数**同一个基准**（那边是排版，这边是
// 翻页；两处各写一遍式子迟早会漂）。行数 < 1 时抬到 1。
static int olVisRows(int y0, int maxY) {
    int vis = (maxY - y0 + LINE_SPACING - 1) / LINE_SPACING;
    return vis < 1 ? 1 : vis;
}
static int olProjectRows() { return olVisRows(FONT_H + 8 + LINE_SPACING + 2, outlineListMaxY()); }
static int olTagMgrRows() { return olVisRows(FONT_H + 6 + LINE_SPACING, STATUS_Y); }
static int olBookmarkRows() { return olVisRows(FONT_H + 8 + LINE_SPACING, STATUS_Y); }
static int olHelpRows() {   // 帮助是固定尺寸的小浮层（与 drawHelp 同式）
    const int boxH = 250, boxY = (SCREEN_H - boxH) / 2;
    const int contentY = (boxY + 8 + g_font.ascent()) + g_font.descent() + 10;
    return olVisRows(contentY, boxY + boxH - 8);
}

static ListView olListView(int sel, int first, int count, int rows, int top) {
    ListView lv;
    lv.sel = sel;
    lv.first = first;
    lv.count = count;
    lv.rows = rows;
    lv.top = top;
    lv.itemH = LINE_SPACING;
    return lv;
}

static bool olApplyListKey(int key, int &sel, int &scroll, int count, int rows, int top) {
    if (key == 'j') key = KEY_DOWN;
    else if (key == 'k') key = KEY_UP;
    ListView lv = olListView(sel, scroll, count, rows, top);
    if (!listViewKey(lv, key)) return false;
    listViewFollow(lv);
    sel = lv.sel;
    scroll = lv.first;
    return true;
}

// ── Screen entry ─────────────────────────────────────────────────────────
void screen_outline_init() {
    mkdir(OUTLINE_DIR, 0777);

    // Handle post-editor file copy
    bool returningFromEditor = !g.pendingOutlineTarget.empty();
    if (returningFromEditor) {
        std::string tempPath = std::string("/sdcard/pjournal/") + g.pendingJournalFile;
        std::string content = readContentFile(tempPath);
        if (!content.empty()) {
            // Strip journal header
            std::string body = extractBody(content);
            if (body.empty()) body = content;

            // Ensure target directory exists
            size_t slash = g.pendingOutlineTarget.rfind('/');
            if (slash != std::string::npos) {
                std::string dir = g.pendingOutlineTarget.substr(0, slash);
                mkdir(dir.c_str(), 0777);
            }

            FILE *f = fopen(g.pendingOutlineTarget.c_str(), "w");
            if (f) {
                fwrite(body.data(), 1, body.size(), f);
                fclose(f);
            }
        }
        // Always clean up temp file (even if editor was cancelled)
        remove(tempPath.c_str());
        g.pendingOutlineTarget.clear();
        g.pendingJournalFile.clear();
        // Reload outline data after returning from editor
        if (g.curProject >= 0 && g.curProject < (int)g.projects.size()) {
            loadOutline();
            rebuildFilter();
        }
    }

    g.mode = returningFromEditor ? M_BROWSE : M_PROJECTS;
    if (!returningFromEditor) {
        g.sel = 0;
        g.scroll = 0;
        g.curProject = -1;
    }
    g.editBuf.clear();
    g.editCur = 0;
    g.imeActive = false;
    g.pendingLevel = 0;
    g.insertAfter = -1;
    g.filterText.clear();
    g.editNoteIdx = -1;
    g.editingTitle = false;
    g_ime.setActive(false);

    if (!returningFromEditor) {
        g.nodes = nullptr;
        g.nodeCount = 0;
    }

    g.projects = listProjects();
    g_filteredIdx.clear();
}

// ── Main handle ──────────────────────────────────────────────────────────
// P3c：下面这些函数是从 1263 行的 screen_outline_handle 里**原样搬出来**的各个
// `if (g.mode == …) { … }` 块。与计划模式不同，这里的块**不是互斥的**（M_ITEM_MENU /
// M_BROWSE 各有两块，且前面的块可能落空往下走），所以没做成 else-if 分发表，而是每个
// 函数：命中并处理了 → 返回 true 并把目标 AppState 写进 out；没命中 → false，由调用方
// 接着试下一块。`key` 按引用传：块里（以及调用方前面）会把点按翻译成键码，后面的块
// 要看到同一个 key。逻辑一行未改，只把 `return APP_X;` 换成 `out = APP_X; return true;`。

static bool olHandleBrowseLongPress(int &key, AppState &out) {
    if (g.mode == M_BROWSE && key == KEY_TOUCH_LONG && g.filterText.empty()) {
        int lx = 0, ly = 0;
        if (input_tap_xy(&lx, &ly)) {
            int top0 = FONT_H + 8 + LINE_SPACING - g_font.ascent();
            int r = (ly >= top0) ? (ly - top0) / LINE_SPACING : -1;
            if (r >= 0 && r < outlineListRows() && (g.scroll + r) < (int)g.nodeCount) {
                g.sel = g.scroll + r;
                g.itemMenuSel = 0;
                g.mode = M_ITEM_MENU;
                ui_render_begin_overlay();   // 叠在上一帧上（见 ui_render.h）
                drawOutlineItemMenu();
                out = APP_OUTLINE; return true;
            }
        }
    }
    return false;
}

static bool olHandleItemMenuTap(int &key, AppState &out) {
    if (g.mode == M_ITEM_MENU && (key == 0x0A || key == 0x0D)) {
        int tx = 0, ty = 0;
        if (input_tap_xy(&tx, &ty)) {
            int bx, by, bw, bh;
            outlineItemMenuBoxRect(&bx, &by, &bw, &bh);
            if (tx < bx || tx >= bx + bw || ty < by || ty >= by + bh) {
                g.mode = M_BROWSE;          // 点浮层外 = 关掉
                drawOutline(); ui_commit();
                out = APP_OUTLINE; return true;
            }
            int top0 = outlineItemMenuRowY(by, 0) - g_font.ascent();
            int r = (ty >= top0) ? (ty - top0) / LINE_SPACING : -1;
            if (r < 0 || r >= kOlMenuCount) key = 0;   // 落在行缝里：只重绘
            else g.itemMenuSel = r;
        }
    }
    return false;
}

static bool olHandleAddProject(int &key, AppState &out) {
    if (g.mode == M_ADD_PROJECT) {
        if (g.imeActive && key != 0) {
            std::string imeOut;
            if (imeFieldKeyText(g_ime, key, /*multiline=*/false, imeOut)) {
                if (!imeOut.empty()) {
                    imeFieldInsert(olEditField(), imeOut);
                }
                drawInputOverlay("新建项目"); out = APP_OUTLINE; return true;
            }
        }
        if (key == KEY_IME_TOGGLE) {
            g.imeActive = !g.imeActive; g_ime.setActive(g.imeActive);
            drawInputOverlay("新建项目"); out = APP_OUTLINE; return true;
        }
        if (key == 0x1B) {
            g.mode = (g.curProject >= 0) ? M_BROWSE : M_PROJECTS;
            g.imeActive = false; g_ime.setActive(false);
        } else if (key == 0x0A || key == 0x0D) {
            if (!g.editBuf.empty()) {
                std::string dir = std::string(OUTLINE_DIR) + "/" + g.editBuf;
                mkdir(OUTLINE_DIR, 0777);
                mkdir(dir.c_str(), 0777);
                JsonValue data;
                data.set("nodes", JsonValue::array());
                JsonValue::saveToFile(dir + "/project.json", data);
                g.projects = listProjects();
                for (size_t i = 0; i < g.projects.size(); i++)
                    if (g.projects[i] == g.editBuf) { g.curProject = (int)i; break; }
            }
            g.mode = (g.curProject >= 0) ? M_BROWSE : M_PROJECTS;
            g.imeActive = false; g_ime.setActive(false);
            loadOutline();
            rebuildFilter();
        } else if (key == 0x7F || key == 0x08) {
            imeFieldBackspace(olEditField());
        } else if (key == KEY_LEFT) {
            imeFieldMoveLeft(olEditField());
        } else if (key == KEY_RIGHT) {
            imeFieldMoveRight(olEditField());
        } else if (key >= 0x20 && key <= 0x7E) {
            imeFieldInsert(olEditField(), std::string(1, (char)key));
        }
        drawInputOverlay("新建项目");
        out = APP_OUTLINE; return true;
    }
    return false;
}

static bool olHandleFilter(int &key, AppState &out) {
    if (g.mode == M_FILTER) {
        // Esc always exits filter mode, even when IME is active
        if (key == 0x1B) {
            g.mode = M_BROWSE; g.imeActive = false; g_ime.setActive(false);
            g.filterText.clear(); rebuildFilter();
            drawOutline(); ui_commit(); out = APP_OUTLINE; return true;
        }
        // Backspace: if IME is composing, let it handle; otherwise delete from filterText
        if ((key == 0x7F || key == 0x08) && g.imeActive && g_ime.composing()) {
            std::string imeOut;
            g_ime.handleKey(key, imeOut);
            drawOutline(); ui_commit(); out = APP_OUTLINE; return true;
        }
        if ((key == 0x7F || key == 0x08) && (!g.imeActive || !g_ime.composing())) {
            if (imeFieldBackspace(olFilterField())) rebuildFilter();
            drawOutline(); ui_commit(); out = APP_OUTLINE; return true;
        }
        if (g.imeActive && key != 0) {
            std::string imeOut;
            if (imeFieldKeyText(g_ime, key, /*multiline=*/false, imeOut)) {
                if (!imeOut.empty()) { imeFieldInsert(olFilterField(), imeOut); rebuildFilter(); }
                drawOutline(); ui_commit(); out = APP_OUTLINE; return true;
            }
            // IME consumed the key (still composing) — don't add to filterText
            drawOutline(); ui_commit(); out = APP_OUTLINE; return true;
        }
        if (key == KEY_IME_TOGGLE) {
            g.imeActive = !g.imeActive; g_ime.setActive(g.imeActive);
            drawOutline(); ui_commit(); out = APP_OUTLINE; return true;
        }
        if (key == 0x0A || key == 0x0D) {
            g.mode = M_BROWSE; g.imeActive = false; g_ime.setActive(false);
        } else if (key >= 0x20 && key <= 0x7E) {
            imeFieldInsert(olFilterField(), std::string(1, (char)key)); rebuildFilter();
        }
        drawOutline(); ui_commit();
        out = APP_OUTLINE; return true;
    }
    return false;
}

static bool olHandleDetail(int &key, AppState &out, ScreenContext &ctx) {
    if (g.mode == M_DETAIL) {
        if (key == 0x1B || key == 'q' || key == 'Q') {
            g.mode = M_BROWSE;
        } else if (key == KEY_UP) {
            if (g.detailField > 0) g.detailField--;
        } else if (key == KEY_DOWN) {
            if (g.detailField < 4) g.detailField++;
        } else if (key == 0x0A || key == 0x0D) {
            if (!g.nodes || g.detailNodeIdx < 0 || g.detailNodeIdx >= (int)g.nodeCount) { g.mode = M_BROWSE; }
            else {
                auto &node = (*g.nodes)[g.detailNodeIdx];
                if (g.detailField == 0) {
                    g.editNoteIdx = g.detailNodeIdx;
                    g.editingTitle = true;
                    g.editingKeyword = false;
                    g.editBuf = node["title"].asString();
                    g.editCur = (int)g.editBuf.length();
                    g.imeActive = true; g_ime.setActive(true);
                    g.mode = M_EDIT_NOTE;
                } else if (g.detailField == 1) {
                    // status picker
                    openOutlinePicker(1);
                } else if (g.detailField == 2) {
                    g.editNoteIdx = g.detailNodeIdx;
                    g.editingKeyword = true;
                    g.editBuf = node["keywords"].asString();
                    g.editCur = (int)g.editBuf.length();
                    g.imeActive = true; g_ime.setActive(true);
                    g.mode = M_EDIT_NOTE;
                } else if (g.detailField == 3) {
                    // Open multi-line note editor
                    g.editNoteIdx = g.detailNodeIdx;
                    std::string note = node["note"].asString();
                    g.noteLines.clear();
                    size_t npos = 0;
                    while (npos < note.length()) {
                        size_t nl = note.find('\n', npos);
                        g.noteLines.push_back((nl == std::string::npos) ? note.substr(npos) : note.substr(npos, nl - npos));
                        if (nl == std::string::npos) break;
                        npos = nl + 1;
                    }
                    if (g.noteLines.empty()) g.noteLines.push_back("");
                    g.noteRow = 0; g.noteCol = (int)g.noteLines[0].length();
                    g.noteScroll = 0; g.noteVrowsDirty = true;
                    g.imeActive = true; g_ime.setActive(true);
                    g.mode = M_EDIT_NOTE_ML;
                } else if (g.detailField == 4) {
                    // tags picker
                    openOutlinePicker(4);
                }
            }
        } else if (key == 's' || key == 'S') {
            g.summaryNodeIdx = g.detailNodeIdx;
            g.summaryScroll = 0;
            g.mode = M_SUMMARY;
        } else if ((key == 'f' || key == 'F') && g.nodes && g.detailNodeIdx >= 0 && (size_t)g.detailNodeIdx < g.nodeCount) {
            auto &node = (*g.nodes)[g.detailNodeIdx];
            std::string file = node["file"].asString();
            std::string title = node["title"].asString();
            if (file.empty()) {
                file = safeFilename(title);
                node.set("file", file);
                saveOutline();
            }
            std::string fullPath = ensureContentFile(g.projects[g.curProject], file);
            std::string content = readContentFile(fullPath);
            std::string body = extractBody(content);
            if (body.empty()) body = content;
            ctx.editContent = body;
            g.pendingJournalFile = std::string("__outline_") + file;
            ctx.editFilename = g.pendingJournalFile;
            g.pendingOutlineTarget = fullPath;
            ctx.prevState = APP_OUTLINE;
            ctx.nextState = APP_EDITOR;
            out = APP_EDITOR; return true;
        }
        if (key == '?') {
            g.helpScroll = 0;
            g.helpPrevMode = M_DETAIL;
            g.mode = M_HELP;
            ui_render_begin_overlay();   // 叠在上一帧上（见 ui_render.h）
            drawHelp();
            ui_commit();
            out = APP_OUTLINE; return true;
        }

        drawOutlineDetail();
        out = APP_OUTLINE; return true;
    }
    return false;
}

static bool olHandleEditNote(int &key, AppState &out) {
    if (g.mode == M_EDIT_NOTE) {
        if (g.imeActive && key != 0) {
            std::string imeOut;
            if (imeFieldKeyText(g_ime, key, /*multiline=*/false, imeOut)) {
                if (!imeOut.empty()) {
                    imeFieldInsert(olEditField(), imeOut);
                }
                drawInputOverlay(g.editingTitle ? "编辑标题" : (g.editingKeyword ? "编辑关键词" : "编辑备注")); out = APP_OUTLINE; return true;
            }
        }
        if (key == KEY_IME_TOGGLE) {
            g.imeActive = !g.imeActive; g_ime.setActive(g.imeActive);
            drawInputOverlay(g.editingTitle ? "编辑标题" : (g.editingKeyword ? "编辑关键词" : "编辑备注")); out = APP_OUTLINE; return true;
        }
        if (key == 0x1B) {
            g.mode = M_DETAIL; g.imeActive = false; g_ime.setActive(false);
        } else if (key == 0x0A || key == 0x0D) {
            if (g.editNoteIdx >= 0 && g.nodes && (size_t)g.editNoteIdx < g.nodeCount) {
                if (g.editingTitle)
                    (*g.nodes)[g.editNoteIdx].set("title", g.editBuf);
                else if (g.editingKeyword)
                    (*g.nodes)[g.editNoteIdx].set("keywords", g.editBuf);
                else
                    (*g.nodes)[g.editNoteIdx].set("note", g.editBuf);
                saveOutline();
            }
            g.mode = M_DETAIL; g.imeActive = false; g_ime.setActive(false);
        } else if (key == 0x7F || key == 0x08) {
            imeFieldBackspace(olEditField());
        } else if (key == KEY_LEFT) {
            imeFieldMoveLeft(olEditField());
        } else if (key == KEY_RIGHT) {
            imeFieldMoveRight(olEditField());
        } else if (key >= 0x20 && key <= 0x7E) {
            imeFieldInsert(olEditField(), std::string(1, (char)key));
        }
        drawInputOverlay(g.editingTitle ? "编辑标题" : (g.editingKeyword ? "编辑关键词" : "编辑备注"));
        out = APP_OUTLINE; return true;
    }
    return false;
}

static bool olHandlePicker(int &key, AppState &out) {
    if (g.mode == M_PICKER) {
        // 标签选择器在一条标签都没有时是空的（见 openOutlinePicker 的 fieldIdx==4）。
        // 空 vector 上按回车/空格会往 pickerToggled 里塞下标 0，随后按 y
        // 就是 pickerOpts[0] 越界读 → 崩。空选择器直接退回详情页。
        if (g.pickerOpts.empty()) {
            g.mode = M_DETAIL;
            drawOutlineDetailInner();
            ui_commit();
            out = APP_OUTLINE;
            return true;
        }
        if (key == 0x1B || key == 'q' || key == 'Q') {
            g.mode = M_DETAIL;
        } else if (key == KEY_UP) {
            if (g.pickerSel > 0) g.pickerSel--;
        } else if (key == KEY_DOWN) {
            if (g.pickerSel < (int)g.pickerOpts.size() - 1) g.pickerSel++;
        } else if (key == 0x0A || key == 0x0D || key == ' ') {
            auto &node = (*g.nodes)[g.detailNodeIdx];
            if (g.pickerField == 1) {  // status
                node.set("status", g.pickerOpts[g.pickerSel].value);
                saveOutline();
                g.mode = M_DETAIL;
            } else if (g.pickerField == 4) {  // tags - toggle
                if (g.pickerToggled.count(g.pickerSel))
                    g.pickerToggled.erase(g.pickerSel);
                else
                    g.pickerToggled.insert(g.pickerSel);
            }
        } else if (key == 'y' || key == 'Y') {
            // confirm tags selection
            if (g.pickerField == 4) {
                auto &node = (*g.nodes)[g.detailNodeIdx];
                JsonValue newTags = JsonValue::array();
                for (int idx : g.pickerToggled)
                    newTags.pushBack(g.pickerOpts[idx].value);
                node.set("tags", newTags);
                saveOutline();
                g.mode = M_DETAIL;
            }
        }
        // Draw picker overlay (GTD-style: no title, popup on detail view)
        {
            drawOutlineDetailInner();
            int n = (int)g.pickerOpts.size();
            if (n == 0) { ui_commit(); out = APP_OUTLINE; return true; }
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
            bool isTagPicker = (g.pickerField == 4);
            for (int i = 0; i < vis; i++) {
                int oi = scroll + i;
                int iy = boxY + 27 + i * LINE_SPACING;
                bool s = (oi == g.pickerSel);
                bool toggled = isTagPicker && g.pickerToggled.count(oi);
                std::string display;
                if (isTagPicker) display = toggled ? "[x]" : "[ ]";
                display += g.pickerOpts[oi].display;
                if (s) {
                    u8g2_SetDrawColor(g_u8g2, 0);
                    u8g2_DrawBox(g_u8g2, boxX + 4, iy - g_font.ascent(), boxW - 8, FONT_H);
                    u8g2_SetDrawColor(g_u8g2, 1);
                    // 同 GTD 选择器：TTF 渲染器不吃 u8g2 的 draw color，反色要显式传 invert。
                    g_font.drawText(boxX + 8, iy, display.c_str(), true);
                    u8g2_SetDrawColor(g_u8g2, 0);
                } else {
                    g_font.drawText(boxX + 8, iy, display.c_str(), false);
                }
            }
            ui_commit();
        }
        out = APP_OUTLINE; return true;
    }
    return false;
}

static bool olHandleTagMgr(int &key, AppState &out) {
    if (g.mode == M_TAG_MGR) {
        if (key == 0x1B || key == 'q' || key == 'Q') {
            g.mode = M_BROWSE;
        } else if (key == KEY_UP || key == KEY_DOWN || key == KEY_PAGE_UP || key == KEY_PAGE_DOWN ||
                   key == KEY_HOME || key == KEY_END || key == 'j' || key == 'k') {
            int scroll = 0;
            olApplyListKey(key, g.tagMgrSel, scroll, (int)g.tagList.size(), olTagMgrRows(),
                           FONT_H + 6 + LINE_SPACING - g_font.ascent());
        } else if (key == 'a' || key == 'A') {
            g.mode = M_ADD_TAG;
            g.editBuf.clear(); g.editCur = 0;
            g.imeActive = true; g_ime.setActive(true);
        } else if ((key == 'd' || key == 'D') && g.tagMgrSel < (int)g.tagList.size()) {
            std::string name = g.tagList[g.tagMgrSel];
            // Remove from stored list
            auto &arr = g.outlineData["tags"];
            for (int i = (int)arr.size() - 1; i >= 0; i--)
                if (arr[i].asString() == name) arr.elements.erase(arr.elements.begin() + i);
            // Remove from nodes
            if (g.nodes) {
                for (size_t i = 0; i < g.nodeCount; i++) {
                    auto &tt = (*g.nodes)[i]["tags"];
                    if (tt.isArray()) {
                        for (int j = (int)tt.size() - 1; j >= 0; j--)
                            if (tt[j].asString() == name) tt.elements.erase(tt.elements.begin() + j);
                    }
                }
            }
            saveOutline();
            buildTagList();
            if (g.tagMgrSel >= (int)g.tagList.size()) g.tagMgrSel = (int)g.tagList.size() - 1;
            if (g.tagMgrSel < 0) g.tagMgrSel = 0;
        } else if ((key == 'r' || key == 'R') && g.tagMgrSel < (int)g.tagList.size()) {
            g.mode = M_RENAME_TAG;
            g.renameTargetTag = g.tagList[g.tagMgrSel];
            g.editBuf = g.tagList[g.tagMgrSel];
            g.editCur = (int)g.editBuf.length();
            g.imeActive = true; g_ime.setActive(true);
        } else if ((key == 0x0A || key == 0x0D) && g.tagMgrSel < (int)g.tagList.size()) {
            // Toggle tag filter
            std::string name = g.tagList[g.tagMgrSel];
            bool found = false;
            for (int i = 0; i < (int)g.filterTags.size(); i++) {
                if (g.filterTags[i] == name) { g.filterTags.erase(g.filterTags.begin() + i); found = true; break; }
            }
            if (!found) g.filterTags.push_back(name);
            rebuildFilter();
        }
        // Draw tag manager
        {
            ui_clear(); int y = FONT_H + 6 + LINE_SPACING;
            ui_draw_text(4, FONT_H, "标签管理", false, true);
            u8g2_DrawHLine(g_u8g2, 0, FONT_H + 4, SCREEN_W);
            int maxY = STATUS_Y;
            int vis = (maxY - y + LINE_SPACING - 1) / LINE_SPACING;
            if (vis < 1) vis = 1;
            if (g.tagMgrSel < g.scroll) g.scroll = g.tagMgrSel;
            if (g.tagMgrSel >= g.scroll + vis) g.scroll = g.tagMgrSel - vis + 1;
            for (int i = 0; i < vis && (g.scroll + i) < (int)g.tagList.size(); i++) {
                int idx = g.scroll + i;
                bool s = (idx == g.tagMgrSel);
                char buf[64];
                snprintf(buf, sizeof(buf), "#%s", g.tagList[idx].c_str());
                ui_draw_text(8, y + i * LINE_SPACING, buf, s);
                // Show filter indicator
                bool active = false;
                for (auto &ft : g.filterTags) if (ft == g.tagList[idx]) { active = true; break; }
                if (active) g_font.drawText(SCREEN_W - g_font.textWidth("\xe2\x97\x8f") - 4, y + i * LINE_SPACING, "\xe2\x97\x8f", false);
            }
            if (g.tagList.empty()) ui_draw_text(8, y, "暂无 — 按a添加");
            ui_draw_status("a:添加 d:删除 r:重命名 Enter:筛选 Esc:返回", "");
            ui_commit();
        }
        out = APP_OUTLINE; return true;
    }
    return false;
}

static bool olHandleAddTag(int &key, AppState &out) {
    if (g.mode == M_ADD_TAG || g.mode == M_RENAME_TAG) {
        if (g.imeActive && key != 0) {
            std::string imeOut;
            if (imeFieldKeyText(g_ime, key, /*multiline=*/false, imeOut)) {
                if (!imeOut.empty()) { imeFieldInsert(olEditField(), imeOut); }
                drawInputOverlay(g.mode == M_ADD_TAG ? "添加标签" : "重命名标签"); out = APP_OUTLINE; return true;
            }
        }
        if (key == KEY_IME_TOGGLE) {
            g.imeActive = !g.imeActive; g_ime.setActive(g.imeActive);
            drawInputOverlay(g.mode == M_ADD_TAG ? "添加标签" : "重命名标签"); out = APP_OUTLINE; return true;
        }
        if (key == 0x1B) {
            g.mode = M_TAG_MGR; g.imeActive = false; g_ime.setActive(false);
        } else if (key == 0x0A || key == 0x0D) {
            if (!g.editBuf.empty()) {
                std::string name = g.editBuf;
                if (!name.empty() && name[0] == '#') name = name.substr(1);
                if (!name.empty()) {
                    if (g.mode == M_ADD_TAG) {
                        if (!g.outlineData.has("tags") || !g.outlineData["tags"].isArray())
                            g.outlineData.set("tags", JsonValue::array());
                        auto &arr = g.outlineData["tags"];
                        arr.pushBack(name);
                    } else {
                        // Rename: update stored list and all nodes
                        std::string oldName = g.renameTargetTag;
                        auto &arr = g.outlineData["tags"];
                        for (int i = 0; i < (int)arr.size(); i++)
                            if (arr[i].asString() == oldName) arr.elements[i] = JsonValue(name);
                        if (g.nodes) {
                            for (size_t i = 0; i < g.nodeCount; i++) {
                                auto &tt = (*g.nodes)[i]["tags"];
                                if (tt.isArray()) {
                                    for (int j = 0; j < (int)tt.size(); j++)
                                        if (tt[j].asString() == oldName) tt.elements[j] = JsonValue(name);
                                }
                            }
                        }
                    }
                    saveOutline();
                    buildTagList();
                }
            }
            g.mode = M_TAG_MGR; g.imeActive = false; g_ime.setActive(false);
        } else if (key == 0x7F || key == 0x08) {
            imeFieldBackspace(olEditField());
        } else if (key >= 0x20 && key <= 0x7E) { imeFieldInsert(olEditField(), std::string(1, (char)key)); }
        drawInputOverlay(g.mode == M_ADD_TAG ? "添加标签" : "重命名标签");
        out = APP_OUTLINE; return true;
    }
    return false;
}

static bool olHandleSummary(int &key, AppState &out) {
    if (g.mode == M_SUMMARY) {
        if (key == 0x1B || key == 'q' || key == 'Q') {
            g.mode = M_BROWSE;
        } else if (key == KEY_UP) {
            if (g.summaryScroll > 0) g.summaryScroll--;
            drawOutline(); drawSummary();
            out = APP_OUTLINE; return true;
        } else if (key == KEY_DOWN) {
            g.summaryScroll++;
            drawOutline(); drawSummary();
            out = APP_OUTLINE; return true;
        }
        drawOutline(); drawSummary();
        out = APP_OUTLINE; return true;
    }
    return false;
}

static bool olHandleHelp(int &key, AppState &out) {
    if (g.mode == M_HELP) {
        if (key == 0x1B || key == 'q' || key == 'Q' || key == 0x0A || key == 0x0D) {
            g.mode = g.helpPrevMode;
        } else if (key == KEY_UP) {
            if (g.helpScroll > 0) g.helpScroll--;
        } else if (key == KEY_DOWN) {
            g.helpScroll++;
        } else if (key == KEY_PAGE_UP) {
            g.helpScroll -= olHelpRows();
            if (g.helpScroll < 0) g.helpScroll = 0;
        } else if (key == KEY_PAGE_DOWN) {
            g.helpScroll += olHelpRows();   // 上限在画的时候夹住
        } else if (key == KEY_LEFT) {
            if (g.helpScroll > 5) g.helpScroll -= 5; else g.helpScroll = 0;
        } else if (key == KEY_RIGHT) {
            g.helpScroll += 5;
        }
        ui_render_begin_overlay();   // 叠在上一帧上（见 ui_render.h）
        drawHelp();
        out = APP_OUTLINE; return true;
    }
    return false;
}

static bool olHandleBookmarkMgr(int &key, AppState &out) {
    if (g.mode == M_BOOKMARK_MGR) {
        auto &bmArr = g.outlineData["bookmarks"];
        int bmCount = bmArr.isArray() ? (int)bmArr.size() : 0;
        if (key == 0x1B || key == 'q' || key == 'Q') {
            g.mode = M_BROWSE;
        } else if (key == KEY_UP || key == KEY_DOWN || key == KEY_PAGE_UP || key == KEY_PAGE_DOWN ||
                   key == KEY_HOME || key == KEY_END || key == 'j' || key == 'k') {
            int scroll = 0;
            // j = 下、k = 上，与全仓其余列表一致；原来这里是反的（j→UP/k→DOWN），
            // 从书签管理切到别的列表手会打结。
            if (key == 'j') key = KEY_DOWN;
            else if (key == 'k') key = KEY_UP;
            olApplyListKey(key, g.bmMgrSel, scroll, bmCount, olBookmarkRows(),
                           FONT_H + 8 + LINE_SPACING - g_font.ascent());
        } else if (key == 0x0A || key == 0x0D) {
            // Jump to bookmarked node
            if (g.bmMgrSel >= 0 && g.bmMgrSel < bmCount && g.nodes) {
                std::string bid = bmArr[g.bmMgrSel]["id"].asString();
                for (size_t i = 0; i < g.nodeCount; i++) {
                    if ((*g.nodes)[i]["id"].asString() == bid) {
                        // Unfold ancestors and select
                        g.foldedNodes.clear();
                        rebuildFilter();
                        // Find in filtered
                        for (int fi = 0; fi < (int)g_filteredIdx.size(); fi++) {
                            if (g_filteredIdx[fi] == (int)i) { g.sel = fi; break; }
                        }
                        g.mode = M_BROWSE;
                        break;
                    }
                }
            }
        } else if ((key == 'd' || key == 'D') && g.bmMgrSel >= 0 && g.bmMgrSel < bmCount) {
            bmArr.elements.erase(bmArr.elements.begin() + g.bmMgrSel);
            if (g.bmMgrSel >= bmCount - 1) g.bmMgrSel = bmCount - 2;
            if (g.bmMgrSel < 0) g.bmMgrSel = 0;
            saveOutline();
        }
        drawBookmarkMgr();
        out = APP_OUTLINE; return true;
    }
    return false;
}

static bool olHandleEditNoteMl(int &key, AppState &out) {
    if (g.mode == M_EDIT_NOTE_ML) {
        if (g.imeActive && key != 0) {
            std::string imeOut;
            if (imeFieldKeyText(g_ime, key, /*multiline=*/false, imeOut)) {
                if (!imeOut.empty()) {
                    imeFieldInsert(olNoteField(), imeOut);
                    g.noteVrowsDirty = true;
                }
                goto drawNoteEditor;
            }
        }
        if (key == KEY_IME_TOGGLE) {
            g.imeActive = !g.imeActive; g_ime.setActive(g.imeActive);
            goto drawNoteEditor;
        }
        if (key == 0x1B) {
            // Save and close
            if (g.editNoteIdx >= 0 && g.nodes && (size_t)g.editNoteIdx < g.nodeCount) {
                std::string noteText;
                for (int i = 0; i < (int)g.noteLines.size(); i++) {
                    if (i > 0) noteText += '\n';
                    noteText += g.noteLines[i];
                }
                (*g.nodes)[g.editNoteIdx].set("note", noteText);
                saveOutline();
            }
            g.mode = M_DETAIL; g.imeActive = false; g_ime.setActive(false);
            drawOutlineDetail(); out = APP_OUTLINE; return true;
        }
        if (key == 0x0A || key == 0x0D) {
            // New line
            std::string rest = g.noteLines[g.noteRow].substr(g.noteCol);
            g.noteLines[g.noteRow] = g.noteLines[g.noteRow].substr(0, g.noteCol);
            g.noteRow++;
            g.noteLines.insert(g.noteLines.begin() + g.noteRow, rest);
            g.noteCol = 0;
            g.noteVrowsDirty = true;
        } else if (key == 0x7F || key == 0x08) {
            if (g.noteCol > 0) {
                imeFieldBackspace(olNoteField());
                g.noteVrowsDirty = true;
            } else if (g.noteRow > 0) {
                // Join with previous line
                int prevLen = (int)g.noteLines[g.noteRow - 1].length();
                g.noteLines[g.noteRow - 1] += g.noteLines[g.noteRow];
                g.noteLines.erase(g.noteLines.begin() + g.noteRow);
                g.noteRow--;
                g.noteCol = prevLen;
                g.noteVrowsDirty = true;
            }
        } else if (key == KEY_UP) {
            if (g.noteRow > 0) { g.noteRow--; g.noteCol = std::min(g.noteCol, (int)g.noteLines[g.noteRow].length()); }
        } else if (key == KEY_DOWN) {
            if (g.noteRow < (int)g.noteLines.size() - 1) { g.noteRow++; g.noteCol = std::min(g.noteCol, (int)g.noteLines[g.noteRow].length()); }
        } else if (key == KEY_LEFT) {
            if (g.noteCol > 0) imeFieldMoveLeft(olNoteField());
        } else if (key == KEY_RIGHT) {
            if (g.noteCol < (int)g.noteLines[g.noteRow].length()) imeFieldMoveRight(olNoteField());
        } else if (key == '\t' || (key == KEY_CTRL_ENTER)) {
            // Tab or Ctrl+Enter = save
            if (g.editNoteIdx >= 0 && g.nodes && (size_t)g.editNoteIdx < g.nodeCount) {
                std::string noteText;
                for (int i = 0; i < (int)g.noteLines.size(); i++) {
                    if (i > 0) noteText += '\n';
                    noteText += g.noteLines[i];
                }
                (*g.nodes)[g.editNoteIdx].set("note", noteText);
                saveOutline();
            }
            g.mode = M_DETAIL; g.imeActive = false; g_ime.setActive(false);
            drawOutlineDetail(); out = APP_OUTLINE; return true;
        } else if (key >= 0x20 && key <= 0x7E) {
            imeFieldInsert(olNoteField(), std::string(1, (char)key));
            g.noteVrowsDirty = true;
        }
        drawNoteEditor:
        {
            // Draw note editor
            ui_clear();
            ui_draw_text(4, g_font.ascent(), "备注编辑", false, true);
            u8g2_DrawHLine(g_u8g2, 0, FONT_H + 4, SCREEN_W);
            if (g.noteVrowsDirty) {
                g.noteVrows.clear();
                for (int li = 0; li < (int)g.noteLines.size(); li++) {
                    if (g.noteLines[li].empty()) { g.noteVrows.push_back({li, 0, 0}); continue; }
                    int pos = 0, len = (int)g.noteLines[li].length();
                    while (pos < len) {
                        int cw = 0, end = pos, lastBreak = -1;
                        while (end < len) {
                            unsigned char c = (unsigned char)g.noteLines[li][end];
                            int cc = 1;
                            if (c >= 0x80 && (c & 0xE0) == 0xC0) cc = 2;
                            else if (c >= 0x80 && (c & 0xF0) == 0xE0) cc = 3;
                            else if (c >= 0x80 && (c & 0xF8) == 0xF0) cc = 4;
                            int charW = g_font.textWidth(g.noteLines[li].substr(end, cc).c_str());
                            if (cw + charW > SCREEN_W - 8) break;
                            cw += charW;
                            if (c == ' ') lastBreak = end + 1;
                            end += cc;
                        }
                        if (end >= len) { g.noteVrows.push_back({li, pos, len}); break; }
                        if (lastBreak > pos) { g.noteVrows.push_back({li, pos, lastBreak}); pos = lastBreak; }
                        else { g.noteVrows.push_back({li, pos, end}); pos = end; }
                    }
                }
                g.noteVrowsDirty = false;
            }
            int contentY = FONT_H + 8 + LINE_SPACING;
            int maxY = olVkActive() ? editorVkTop() - 4
                                    : (g_ime.composing() ? imeStatusPanelTopY() : STATUS_Y);
            int vis = (maxY - contentY) / LINE_SPACING;
            if (vis < 1) vis = 1;
            int cursorVrow = 0;
            for (int i = 0; i < (int)g.noteVrows.size(); i++) {
                if (g.noteVrows[i].lineIdx == g.noteRow && g.noteCol >= g.noteVrows[i].start && g.noteCol <= g.noteVrows[i].end) { cursorVrow = i; break; }
            }
            if (cursorVrow < g.noteScroll) g.noteScroll = cursorVrow;
            if (cursorVrow >= g.noteScroll + vis) g.noteScroll = cursorVrow - vis + 1;
            for (int i = 0; i < vis && (g.noteScroll + i) < (int)g.noteVrows.size(); i++) {
                auto &vr = g.noteVrows[g.noteScroll + i];
                int ly = contentY + i * LINE_SPACING;
                std::string text = g.noteLines[vr.lineIdx].substr(vr.start, vr.end - vr.start);
                ui_draw_text(4, ly, text.c_str(), false);
            }
            {
                auto &vr = g.noteVrows[cursorVrow];
                std::string before = g.noteLines[vr.lineIdx].substr(vr.start, g.noteCol - vr.start);
                int cx = 4 + g_font.textWidth(before.c_str());
                int cy = contentY + (cursorVrow - g.noteScroll) * LINE_SPACING;
                u8g2_SetDrawColor(g_u8g2, 0);
                u8g2_DrawBox(g_u8g2, cx, cy + 4, 8, 3);
                u8g2_SetDrawColor(g_u8g2, 1);
            }
            u8g2_SetDrawColor(g_u8g2, 0);
            bool vkNote = drawInputPanel(false);
            ui_draw_status("Enter换行 Tab保存 Esc返回",
                           vkNote ? "" : imeStatusLabel(g.imeActive).c_str());
            if (vkNote) editorVkDrawIcon();   // 必须在状态栏之后，否则被白底盖掉
            ui_commit();
        }
        out = APP_OUTLINE; return true;
    }
    return false;
}

static bool olHandleConfirm(int &key, AppState &out, ScreenContext &ctx) {
    if (g.mode == M_CONFIRM) {
        ui_clear();
        ui_draw_text_centered(SCREEN_H / 2, g.confirmMsg.c_str(), false, true);
        ui_draw_status("Enter确认 ESC取消", "");
        ui_commit();
        if (key == 0x0A || key == 0x0D) {
            // Confirmed
            if (g.confirmAction == 1 && g.confirmIdx >= 0 && g.nodes && (size_t)g.confirmIdx < g.nodeCount) {
                // Delete heading + associated file
                auto &node = (*g.nodes)[g.confirmIdx];
                std::string file = node["file"].asString();
                if (!file.empty() && g.curProject >= 0 && g.curProject < (int)g.projects.size()) {
                    std::string fpath = std::string(OUTLINE_DIR) + "/" + g.projects[g.curProject] + "/" + file;
                    remove(fpath.c_str());
                }
                g.nodes->erase(g.nodes->begin() + g.confirmIdx);
                g.nodeCount = g.nodes->size();
                if (g.sel >= (int)g.nodeCount) g.sel = (int)g.nodeCount - 1;
                if (g.sel < 0) g.sel = 0;
                saveOutline();
                rebuildFilter();
                // 浮动提示（不清屏、不阻塞），与写作模式里删日记同一套：
                // 删完屏上就是列表、被删项已不在，浮在列表上正好（见 ui_helpers.h）。
                ui_toast_show("已删除");
            } else if (g.confirmAction == 2 && g.confirmIdx >= 0 && g.confirmIdx < (int)g.projects.size()) {
                // Delete project
                std::string dir = std::string(OUTLINE_DIR) + "/" + g.projects[g.confirmIdx];
                DIR *d = opendir(dir.c_str());
                if (d) {
                    struct dirent *ent;
                    while ((ent = readdir(d)) != nullptr) {
                        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
                        std::string fp = dir + "/" + ent->d_name;
                        remove(fp.c_str());
                    }
                    closedir(d);
                }
                rmdir(dir.c_str());
                g.projects.erase(g.projects.begin() + g.confirmIdx);
                if (g.sel >= (int)g.projects.size()) g.sel = (int)g.projects.size() - 1;
                if (g.sel < 0) g.sel = 0;
                ui_toast_show("已删除项目");
            } else if (g.confirmAction == 3 && g.confirmIdx >= 0 && g.nodes && (size_t)g.confirmIdx < g.nodeCount) {
                // Clear file association
                auto &node = (*g.nodes)[g.confirmIdx];
                std::string file = node["file"].asString();
                if (!file.empty() && g.curProject >= 0 && g.curProject < (int)g.projects.size()) {
                    std::string fpath = std::string(OUTLINE_DIR) + "/" + g.projects[g.curProject] + "/" + file;
                    remove(fpath.c_str());
                }
                node.set("file", "");
                saveOutline();
                ui_toast_show("已清除关联");
            }
            g.mode = M_BROWSE;
        } else if (key == 0x1B) {
            // Cancelled
            g.mode = (g.confirmAction == 2) ? M_PROJECTS : M_BROWSE;
        }
        out = APP_OUTLINE; return true;
    }
    return false;
}

static bool olHandleProjects(int &key, AppState &out, ScreenContext &ctx) {
    if (g.mode == M_PROJECTS) {
        if (key == 'q' || key == 'Q' || key == 0x1B) {
            g_ime.setActive(false);
            ctx.nextState = APP_MAIN; out = APP_MAIN; return true;
        }
        if (key == KEY_UP || key == KEY_DOWN || key == KEY_PAGE_UP || key == KEY_PAGE_DOWN ||
            key == KEY_HOME || key == KEY_END || key == 'j' || key == 'k') {
            olApplyListKey(key, g.sel, g.scroll, (int)g.projects.size(), olProjectRows(),
                           FONT_H + 8 + LINE_SPACING + 2 - g_font.ascent());
        }
        if (key == 'n' || key == 'N') {
            g.mode = M_ADD_PROJECT;
            g.editBuf.clear(); g.editCur = 0;
            g.imeActive = true; g_ime.setActive(true);
        }
        if (key == 0x0A || key == 0x0D) {
            if (g.sel < (int)g.projects.size()) {
                g.curProject = g.sel;
                g.mode = M_BROWSE;
                g.sel = 0; g.scroll = 0;
                loadOutline();
                rebuildFilter();
            }
        }

        if ((key == 'd' || key == 'D') && g.sel < (int)g.projects.size()) {
            // Ask for confirmation
            g.confirmAction = 2;
            g.confirmIdx = g.sel;
            g.confirmMsg = std::string("删除项目「") + g.projects[g.sel] + "」?";
            g.mode = M_CONFIRM;
        }

        if (key == '?') {
            g.helpScroll = 0;
            g.helpPrevMode = M_PROJECTS;
            g.mode = M_HELP;
            ui_render_begin_overlay();   // 叠在上一帧上（见 ui_render.h）
            drawHelp();
            ui_commit();
            out = APP_OUTLINE; return true;
        }

        drawProjectList(); ui_commit();
        out = APP_OUTLINE; return true;
    }
    return false;
}

static bool olHandleItemMenu(int &key, AppState &out) {
    if (g.mode == M_ITEM_MENU) {
        bool handOff = false;   // 退出浮层并把动作交给下面的键盘逻辑
        if (key == KEY_UP || key == 'k') {
            if (g.itemMenuSel > 0) g.itemMenuSel--;
        } else if (key == KEY_DOWN || key == 'j') {
            if (g.itemMenuSel < kOlMenuCount - 1) g.itemMenuSel++;
        } else if (key == 0x0A || key == 0x0D) {
            int sel = g.itemMenuSel;
            OlMenuAct a = (sel >= 0 && sel < kOlMenuCount) ? kOlMenuActs[sel] : OMA_CANCEL;
            g.mode = M_BROWSE;
            if (a == OMA_CANCEL) {
                drawOutline(); ui_commit();
                out = APP_OUTLINE; return true;
            }
            key = (a == OMA_RENAME)  ? 'r'
                : (a == OMA_SUBTASK) ? 'i'
                : (a == OMA_PROMOTE) ? 'h'
                : (a == OMA_DEMOTE)  ? 'l' : 'd';
            handOff = true;
        } else if (key == 0x1B || key == 'q' || key == 'Q') {
            g.mode = M_BROWSE;
        }
        if (!handOff) {
            ui_render_begin_overlay();   // 叠在上一帧上（见 ui_render.h）
            drawOutlineItemMenu();
            out = APP_OUTLINE; return true;
        }
        // handOff：浮层已关，key 已换成 'r'/'i'/'h'/'l'/'d'，往下走同一条键盘逻辑
    }
    return false;
}

static bool olHandleBrowse(int &key, AppState &out, ScreenContext &ctx) {
    if (g.mode == M_BROWSE) {
        if (key == 'q' || key == 'Q' || key == 0x1B) {
            if (!g.filterTags.empty()) {
                g.filterTags.clear();
                rebuildFilter();
            } else {
                g.mode = M_PROJECTS;
                g.sel = g.curProject >= 0 ? g.curProject : 0;
                g.scroll = 0;
                g.nodes = nullptr; g.nodeCount = 0;
                g_filteredIdx.clear();
                drawProjectList(); ui_commit();
                out = APP_OUTLINE; return true;
            }
        }

        if (key == '\t') {
            // switch project
            if (!g.projects.empty()) {
                g.curProject = (g.curProject + 1) % (int)g.projects.size();
                g.sel = 0; g.scroll = 0;
                loadOutline();
                rebuildFilter();
            }
        }

        if (key == KEY_UP || key == KEY_DOWN || key == KEY_PAGE_UP || key == KEY_PAGE_DOWN ||
            key == KEY_HOME || key == KEY_END) {
            const int n = g.filterText.empty() ? (int)g.nodeCount : (int)g_filteredIdx.size();
            olApplyListKey(key, g.sel, g.scroll, n, outlineListRows(),
                           FONT_H + 8 + LINE_SPACING - g_font.ascent());
        }

        // hjkl: reorder and hierarchy
        if (g.nodes && g.nodeCount > 0 && g.filterText.empty()) {
            int idx = g.sel;
            if (idx >= 0 && (size_t)idx < g.nodeCount) {
                if (key == 'j' && idx > 0) {
                    std::swap((*g.nodes)[idx], (*g.nodes)[idx - 1]);
                    g.sel--;
                    saveOutline();
                } else if (key == 'k' && idx < (int)g.nodeCount - 1) {
                    std::swap((*g.nodes)[idx], (*g.nodes)[idx + 1]);
                    g.sel++;
                    saveOutline();
                } else if (key == 'h') {
                    auto &node = (*g.nodes)[idx];
                    int lvl = node["level"].asInt(0);
                    if (lvl > 0) { node.set("level", lvl - 1); saveOutline(); }
                } else if (key == 'l') {
                    auto &node = (*g.nodes)[idx];
                    int lvl = node["level"].asInt(0);
                    if (idx > 0) {
                        int prevLvl = (*g.nodes)[idx - 1]["level"].asInt(0);
                        if (lvl <= prevLvl) { node.set("level", lvl + 1); saveOutline(); }
                    }
                }
            }
        }

        if (key == 'a' || key == 'A') {
            // add heading at same level as selected, insert after selected item's group
            if (!g.nodes) {
                g.outlineData = JsonValue::object();
                g.outlineData.set("nodes", JsonValue::array());
                g.outlineData.set("bookmarks", JsonValue::array());
                g.outlineData.set("tags", JsonValue::array());
                g.nodes = &g.outlineData["nodes"].elements;
                g.nodeCount = 0;
            }
            int idx = g.filterText.empty() ? g.sel : (g.sel < (int)g_filteredIdx.size() ? g_filteredIdx[g.sel] : -1);
            if (idx >= 0 && (size_t)idx < g.nodeCount) {
                g.pendingLevel = (*g.nodes)[idx]["level"].asInt(0);
                // Find end of current item's group (skip sub-items at higher levels)
                int insertPos = idx + 1;
                while (insertPos < (int)g.nodeCount && (*g.nodes)[insertPos]["level"].asInt(0) > g.pendingLevel)
                    insertPos++;
                g.insertAfter = insertPos - 1;
            } else {
                g.pendingLevel = 0;
                g.insertAfter = -1;
            }
            g.mode = M_ADD_HEADING;
            g.editBuf.clear(); g.editCur = 0;
            g.imeActive = true; g_ime.setActive(true);
        }

        if (key == 'i' || key == 'I') {
            // add sub-heading at next level from selected, insert right after selected
            if (!g.nodes || (int)g.nodeCount == 0) {
                // create first heading
                if (!g.nodes) {
                    g.outlineData = JsonValue::object();
                    g.outlineData.set("nodes", JsonValue::array());
                    g.outlineData.set("bookmarks", JsonValue::array());
                    g.outlineData.set("tags", JsonValue::array());
                    g.nodes = &g.outlineData["nodes"].elements;
                    g.nodeCount = 0;
                }
                g.mode = M_ADD_HEADING;
                g.insertAfter = -1;
            } else {
                int idx = g.filterText.empty() ? g.sel : (g.sel < (int)g_filteredIdx.size() ? g_filteredIdx[g.sel] : -1);
                g.insertAfter = (idx >= 0 && (size_t)idx < g.nodeCount) ? idx : -1;
                g.mode = M_ADD_SUB;
            }
            g.editBuf.clear(); g.editCur = 0;
            g.imeActive = true; g_ime.setActive(true);
        }

        if (key == '/') {
            g.mode = M_FILTER;
            g.imeActive = true; g_ime.setActive(true);
        }

        if (key == 'r' || key == 'R') {
            // Rename heading title
            int idx = g.filterText.empty() ? g.sel : (g.sel < (int)g_filteredIdx.size() ? g_filteredIdx[g.sel] : -1);
            if (idx >= 0 && g.nodes && (size_t)idx < g.nodeCount) {
                g.editNoteIdx = idx;
                g.editingTitle = true;
                g.editBuf = (*g.nodes)[idx]["title"].asString();
                g.editCur = (int)g.editBuf.length();
                g.imeActive = true;
                g_ime.setActive(true);
                g.mode = M_EDIT_NOTE;
                drawInputOverlay("编辑标题");
                ui_commit();
                out = APP_OUTLINE; return true;
            }
        }



        // s: summary
        if ((key == 's' || key == 'S') && g.nodes && g.nodeCount > 0) {
            int idx = g.filterText.empty() ? g.sel : (g.sel < (int)g_filteredIdx.size() ? g_filteredIdx[g.sel] : -1);
            if (idx >= 0 && (size_t)idx < g.nodeCount) {
                g.summaryNodeIdx = idx;
                g.summaryScroll = 0;
                g.mode = M_SUMMARY;
                drawOutline(); drawSummary();
                out = APP_OUTLINE; return true;
            }
        }

        if (key == 0x05) {  // Ctrl+E — export
            std::string md = exportMD();
            time_t now; time(&now); struct tm *tm = localtime(&now);
            char fname[64];
            strftime(fname, sizeof(fname), "/sdcard/outline/export_%Y%m%d_%H%M%S.md", tm);
            FILE *f = fopen(fname, "w");
            if (f) {
                fwrite(md.data(), 1, md.size(), f);
                fclose(f);
                ctx.statusMessage = "已导出";
            }
        }

        if ((key == 'd' || key == 'D') && g.nodes && g.nodeCount > 0) {
            int idx = g.filterText.empty() ? g.sel : (g.sel < (int)g_filteredIdx.size() ? g_filteredIdx[g.sel] : -1);
            if (idx >= 0 && (size_t)idx < g.nodeCount) {
                g.confirmAction = 1;
                g.confirmIdx = idx;
                g.confirmMsg = std::string("删除标题「") + (*g.nodes)[idx]["title"].asString() + "」?";
                g.mode = M_CONFIRM;
            }
        }

        if ((key == 'c' || key == 'C') && g.nodes && g.nodeCount > 0) {
            int idx = g.filterText.empty() ? g.sel : (g.sel < (int)g_filteredIdx.size() ? g_filteredIdx[g.sel] : -1);
            if (idx >= 0 && (size_t)idx < g.nodeCount && !(*g.nodes)[idx]["file"].asString().empty()) {
                g.confirmAction = 3;
                g.confirmIdx = idx;
                g.confirmMsg = std::string("清除「") + (*g.nodes)[idx]["title"].asString() + "」的文件关联?";
                g.mode = M_CONFIRM;
            }
        }

        if (key == 'n' || key == 'N') {
            // new project (without leaving browse mode)
            g.mode = M_ADD_PROJECT;
            g.editBuf.clear(); g.editCur = 0;
            g.imeActive = true; g_ime.setActive(true);
        }

        if (key == 0x0A || key == 0x0D) {
            // Enter: open detail panel
            int idx = g.filterText.empty() ? g.sel : (g.sel < (int)g_filteredIdx.size() ? g_filteredIdx[g.sel] : -1);
            if (idx >= 0 && g.nodes && (size_t)idx < g.nodeCount) {
                g.detailNodeIdx = idx;
                g.detailField = 0;
                g.mode = M_DETAIL;
            }
        }

        // f: associate/open content file
        if (key == 'f' || key == 'F') {
            int idx = g.filterText.empty() ? g.sel : (g.sel < (int)g_filteredIdx.size() ? g_filteredIdx[g.sel] : -1);
            if (idx >= 0 && g.nodes && (size_t)idx < g.nodeCount) {
                auto &node = (*g.nodes)[idx];
                std::string file = node["file"].asString();
                std::string title = node["title"].asString();
                if (file.empty()) {
                    file = safeFilename(title);
                    node.set("file", file);
                    saveOutline();
                }

                std::string fullPath = ensureContentFile(g.projects[g.curProject], file);
                std::string content = readContentFile(fullPath);

                std::string body = extractBody(content);
                if (body.empty()) body = content;

                ctx.editContent = body;
                g.pendingJournalFile = std::string("__outline_") + file;
                ctx.editFilename = g.pendingJournalFile;
                g.pendingOutlineTarget = fullPath;
                ctx.prevState = APP_OUTLINE;
                ctx.nextState = APP_EDITOR;
                out = APP_EDITOR; return true;
            }
        }

        if (key == '?') {
            g.helpScroll = 0;
            g.helpPrevMode = M_BROWSE;
            g.mode = M_HELP;
            ui_render_begin_overlay();   // 叠在上一帧上（见 ui_render.h）
            drawHelp();
            ui_commit();
            out = APP_OUTLINE; return true;
        }

        // z: toggle fold, Z: fold/unfold all
        if (key == 'z' && g.nodes && g.nodeCount > 0) {
            int idx = g.filterText.empty() ? g.sel : (g.sel < (int)g_filteredIdx.size() ? g_filteredIdx[g.sel] : -1);
            if (idx >= 0 && (size_t)idx < g.nodeCount) {
                if (g.foldedNodes.count(idx)) g.foldedNodes.erase(idx);
                else g.foldedNodes.insert(idx);
                rebuildFilter();
            }
        }
        if (key == 'Z') {
            if (g.foldedNodes.empty()) {
                // Fold all nodes that have children
                for (size_t i = 0; i < g.nodeCount; i++) {
                    int lvl = (*g.nodes)[i]["level"].asInt(0);
                    if (i + 1 < g.nodeCount && (*g.nodes)[i + 1]["level"].asInt(0) > lvl)
                        g.foldedNodes.insert((int)i);
                }
            } else {
                g.foldedNodes.clear();
            }
            rebuildFilter();
        }

        // m: toggle bookmark on selected
        if ((key == 'm' || key == 'M') && g.nodes && g.nodeCount > 0) {
            int idx = g.filterText.empty() ? g.sel : (g.sel < (int)g_filteredIdx.size() ? g_filteredIdx[g.sel] : -1);
            if (idx >= 0 && (size_t)idx < g.nodeCount) {
                auto &bmArr = g.outlineData["bookmarks"];
                auto &node = (*g.nodes)[idx];
                std::string nid = node["id"].asString();
                bool found = false;
                for (int i = 0; i < (int)bmArr.size(); i++) {
                    if (bmArr[i]["id"].asString() == nid) {
                        bmArr.elements.erase(bmArr.elements.begin() + i);
                        found = true; break;
                    }
                }
                if (!found) {
                    JsonValue bm;
                    bm.set("id", nid);
                    bm.set("title", node["title"].asString());
                    bm.set("level", node["level"].asInt(0));
                    bmArr.pushBack(bm);
                }
                saveOutline();
            }
        }

        // t: tag manager
        if ((key == 't' || key == 'T') && g.curProject >= 0) {
            buildTagList();
            g.tagMgrSel = 0;
            g.scroll = 0;
            g.mode = M_TAG_MGR;
            ui_clear(); ui_commit();
            out = APP_OUTLINE; return true;
        }

        // b: bookmark manager
        if (key == 'b' || key == 'B') {
            g.bmMgrSel = 0;
            g.scroll = 0;
            g.mode = M_BOOKMARK_MGR;
            drawBookmarkMgr();
            out = APP_OUTLINE; return true;
        }

        drawOutline(); ui_commit();
        out = APP_OUTLINE; return true;
    }
    return false;
}

static bool olHandleAddHeading(int &key, AppState &out) {
    if (g.mode == M_ADD_HEADING || g.mode == M_ADD_SUB) {
        const char *addTitle = (g.mode == M_ADD_SUB) ? "添加子标题" : "添加标题";
        if (g.imeActive && key != 0) {
            std::string imeOut;
            if (imeFieldKeyText(g_ime, key, /*multiline=*/false, imeOut)) {
                if (!imeOut.empty()) {
                    imeFieldInsert(olEditField(), imeOut);
                }
                drawInputOverlay(addTitle); out = APP_OUTLINE; return true;
            }
        }
        if (key == KEY_IME_TOGGLE) {
            g.imeActive = !g.imeActive; g_ime.setActive(g.imeActive);
            drawInputOverlay(addTitle); out = APP_OUTLINE; return true;
        }
        if (key == 0x1B) {
            g.mode = M_BROWSE; g.imeActive = false; g_ime.setActive(false);
            g.insertAfter = -1;
        } else if (key == 0x0A || key == 0x0D) {
            if (!g.editBuf.empty()) {
                if (!g.nodes) {
                    g.outlineData = JsonValue::object();
                    g.outlineData.set("nodes", JsonValue::array());
                    g.outlineData.set("bookmarks", JsonValue::array());
                    g.outlineData.set("tags", JsonValue::array());
                    g.nodes = &g.outlineData["nodes"].elements;
                    g.nodeCount = 0;
                }

                int newLevel = g.pendingLevel;
                if (g.mode == M_ADD_SUB && g.sel < (int)g.nodeCount) {
                    newLevel = (*g.nodes)[g.sel]["level"].asInt(0) + 1;
                }

                JsonValue node;
                node.set("id", makeId());
                node.set("title", g.editBuf);
                node.set("level", newLevel);
                node.set("file", "");
                node.set("note", "");
                node.set("keywords", "");
                node.set("status", "draft");
                node.set("tags", JsonValue::array());
                if (g.insertAfter >= 0 && g.insertAfter < (int)g.nodes->size())
                    g.nodes->insert(g.nodes->begin() + g.insertAfter + 1, node);
                else
                    g.nodes->push_back(node);
                g.nodeCount = g.nodes->size();
                saveOutline();
                // Position cursor on the newly inserted node
                int newIdx = (g.insertAfter >= 0) ? g.insertAfter + 1 : (int)g.nodeCount - 1;
                g.sel = newIdx;
                rebuildFilter();
            }
            g.mode = M_BROWSE; g.imeActive = false; g_ime.setActive(false);
            g.insertAfter = -1;
            g.insertAfter = -1;
        } else if (key == 0x7F || key == 0x08) {
            imeFieldBackspace(olEditField());
        } else if (key == KEY_LEFT) {
            imeFieldMoveLeft(olEditField());
        } else if (key == KEY_RIGHT) {
            imeFieldMoveRight(olEditField());
        } else if (key >= 0x20 && key <= 0x7E) {
            imeFieldInsert(olEditField(), std::string(1, (char)key));
        }
        drawInputOverlay(addTitle);
        out = APP_OUTLINE; return true;
    }
    return false;
}


AppState screen_outline_handle(int key, ScreenContext &ctx) {
    // 切进"要打字"的模式（新建/重命名/编辑备注/筛选）时，自动把虚拟键盘弹出来
    // （没连蓝牙键盘的话）。只在**模式切换**的那一刻自动展示：用户在本模式里手动
    // 点状态栏图标收起后，不会被下一帧又弹回来。与计划模式同款。
    static int s_prevMode = M_PROJECTS;
    if (g.mode != s_prevMode && olVkEditing()) editorVkAutoShow();
    s_prevMode = g.mode;

    // 触摸上下滑的翻页键（主循环不再替写作界面回退成单步）：**列表/长文**按屏翻
    // ——下面的各 olHandleXxx 里接 KEY_PAGE_*；其余模式（详情表单、选择器、条目菜单、
    // 多行备注、摘要）保持原来的单步语义。
    if (key == KEY_PAGE_UP || key == KEY_PAGE_DOWN) {
        const bool pageMode = (g.mode == M_PROJECTS || g.mode == M_BROWSE || g.mode == M_TAG_MGR ||
                               g.mode == M_BOOKMARK_MGR || g.mode == M_HELP);
        if (!pageMode) key = (key == KEY_PAGE_UP) ? KEY_UP : KEY_DOWN;
    }

    // 虚拟键盘（编辑态，与写作/计划模式同一套）：先认状态栏上的键盘开关图标，再认
    // 键盘面板。命中就把点按翻译成普通键码，交给下面既有的输入逻辑——不重复实现
    // 任何输入。key 置 0 时各编辑块照旧重绘，正好把按下反馈刷出去。
    if (olVkEditing()) editorVkSyncBtState();   // 蓝牙键盘连上就自动收起
    if (olVkEditing() && (key == 0x0A || key == 0x0D)) {
        int vx = 0, vy = 0;
        if (input_tap_xy(&vx, &vy)) {
            if (editorVkIconHit(vx, vy)) {
                editorVkSetVisible(!editorVkVisible());
                key = 0;
            } else if (editorVkVisible() && vy >= editorVkTop()) {
                EditorVkHit hit;
                int vk = editorVkHitTest(vx, vy, &hit);
                key = 0;
                if (vk != EVK_NONE) editorVkMarkPressed(hit);
                // EVK_PAGE：换面板已在命中测试里完成，key 保持 0 → 下面照样重绘
                if (vk == EVK_LANG) {
                    // 未开输入法 → 开中文；已开 → 拼音/英文互切（与物理 Ctrl+Space 等价）
                    if (!g.imeActive) { g.imeActive = true; g_ime.setActive(true); }
                    else g_ime.toggleEnglish();
                } else if (vk > 0) {
                    key = vk;   // 普通键：走下面既有的输入逻辑
                }
                // EVK_CTRL/EVK_SHIFT：待发状态已在命中测试内翻转，重绘即反馈
                // EVK_NONE：点在键盘空白处，吞掉本次点按
            }
        }
    }

    // 触摸点按：命中悬浮「+」就把它翻译成对应的普通键码（项目列表 → 'n' 新建项目，
    // 树里 → 'a' 新建标题），下面既有的键盘逻辑照旧执行，不另写一套新建逻辑。
    // input_tap_xy() 读后即清，改写 key 后不会再次进这里，不会递归。
    if ((g.mode == M_PROJECTS || g.mode == M_BROWSE) && (key == 0x0A || key == 0x0D)) {
        int tx = 0, ty = 0;
        if (input_tap_xy(&tx, &ty)) {
            // 判定顺序与计划模式一致：子标题键 → 「+」→ 方向键。
            if (outlineSubFabHit(tx, ty))       key = 'i';
            else if (outlineFabHit(tx, ty))     key = (g.mode == M_PROJECTS) ? 'n' : 'a';
            else {
                int di = outlineDirPadHit(tx, ty);
                if (di >= 0) key = kOutlinePadKey[di];
            }
        }
    }

    // 长按标题行 → 弹编辑菜单（重命名/添加子标题/提升层级/降低层级/删除）。
    // 长按的点落在哪一行就选中哪一行，与键盘"先选中再按 r/i/h/l/d"完全等价：
    // 菜单选中项回车后只是把键码换回来，走下面同一条分支。
    {
        AppState out = APP_OUTLINE;
        if (olHandleBrowseLongPress(key, out)) return out;
    }

    // 编辑菜单里的点按：命中哪一项就选中它再走回车；点浮层外就关掉菜单。
    {
        AppState out = APP_OUTLINE;
        if (olHandleItemMenuTap(key, out)) return out;
    }

    // ── M_ADD_PROJECT ────────────────────────────────────────────────
    {
        AppState out = APP_OUTLINE;
        if (olHandleAddProject(key, out)) return out;
    }

    // ── M_FILTER ─────────────────────────────────────────────────────
    {
        AppState out = APP_OUTLINE;
        if (olHandleFilter(key, out)) return out;
    }

    // ── M_DETAIL ──────────────────────────────────────────────────
    {
        AppState out = APP_OUTLINE;
        if (olHandleDetail(key, out, ctx)) return out;
    }

    // ── M_EDIT_NOTE ──────────────────────────────────────────────────
    {
        AppState out = APP_OUTLINE;
        if (olHandleEditNote(key, out)) return out;
    }

    // ── M_PICKER ─────────────────────────────────────────────────────
    {
        AppState out = APP_OUTLINE;
        if (olHandlePicker(key, out)) return out;
    }

    // ── M_TAG_MGR ────────────────────────────────────────────────────
    {
        AppState out = APP_OUTLINE;
        if (olHandleTagMgr(key, out)) return out;
    }

    // ── M_ADD_TAG / M_RENAME_TAG ─────────────────────────────────────
    {
        AppState out = APP_OUTLINE;
        if (olHandleAddTag(key, out)) return out;
    }


    // ── M_SUMMARY ──────────────────────────────────────────────────
    {
        AppState out = APP_OUTLINE;
        if (olHandleSummary(key, out)) return out;
    }

    // ── M_HELP ─────────────────────────────────────────────────────
    {
        AppState out = APP_OUTLINE;
        if (olHandleHelp(key, out)) return out;
    }

    // ── M_BOOKMARK_MGR ──────────────────────────────────────────────
    {
        AppState out = APP_OUTLINE;
        if (olHandleBookmarkMgr(key, out)) return out;
    }

    // ── M_EDIT_NOTE_ML: multi-line note editor ───────────────────────
    {
        AppState out = APP_OUTLINE;
        if (olHandleEditNoteMl(key, out)) return out;
    }

    // ── M_CONFIRM: confirmation dialog ────────────────────────────────
    {
        AppState out = APP_OUTLINE;
        if (olHandleConfirm(key, out, ctx)) return out;
    }

    // ── M_PROJECTS: project list ─────────────────────────────────────
    {
        AppState out = APP_OUTLINE;
        if (olHandleProjects(key, out, ctx)) return out;
    }

    // ── M_ITEM_MENU: 长按标题弹出的编辑菜单 ──────────────────────────
    // 浮层：↑↓/j k 移动选中项，回车执行，Esc 关掉。动作**把 key 换成对应的普通键码
    // 继续往下走**（重命名='r'、添加子标题='i'、提升层级='h'、降低层级='l'、删除='d'），
    // 与键盘那几个键是同一条实现。
    {
        AppState out = APP_OUTLINE;
        if (olHandleItemMenu(key, out)) return out;
    }

    // ── M_BROWSE: outline tree ───────────────────────────────────────
    {
        AppState out = APP_OUTLINE;
        if (olHandleBrowse(key, out, ctx)) return out;
    }

    // ── M_ADD_HEADING / M_ADD_SUB ────────────────────────────────────
    {
        AppState out = APP_OUTLINE;
        if (olHandleAddHeading(key, out)) return out;
    }

    // Fallback
    drawOutline(); ui_commit();
    return APP_OUTLINE;
}
