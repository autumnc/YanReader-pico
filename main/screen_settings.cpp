#include "screen_settings.h"
#include "font_renderer.h"
#include "settings_manager.h"
#include "font_store.h"
#include "ttf_font.h"
#include "wifi_manager.h"
#include "opds_client.h"
#include "flomo_client.h"
#include "ime/IME.h"
#include "ui/ime_field.h"  // 输入框绑定：落串/退格/光标的 UTF-8 算术（与写作/计划/阅读共用一份）
#include "ui/list_view.h"
#include "editor_vk.h"   // editorVkSetLayout：键盘布局这一行直接改虚拟键盘的键位表
#include "text_sel.h"    // 输入框的触摸选区（三模式共享件）
#include "pcf85063.h"
#include "standby_clock.h"
#include "quick_edit.h"
#include "settings_backup.h"   // 通用分类末尾的「备份设置与记录 / 从备份恢复」
#include "app_async.h"
#include "app_services.h"
#include "read_pico_sd.h"      // 卡在不在（恢复前的提示要分开说）
#include "typing_click.h"
#include "ui_helpers.h"
#include "ui_feedback.h"
#include "input.h"
#include "board.h"
#include <HalDisplay.h>  // 夜间模式：翻全局反色标志
#include <cstdio>
#include <cstring>
#include <ctime>
#include <algorithm>
#include <atomic>
#include <set>
#include <vector>
#include <esp_timer.h>
#include <esp_system.h>   // esp_restart：「从备份恢复」换完文件后重启
#include <esp_sntp.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <sys/stat.h>
#include <strings.h>  // strcasecmp
#include "u8g2_shim.h"

static const char *TAG = "Settings";

// ── Settings state ────────────────────────────────────────────────────────
// 设置项按分类挂进子菜单：顶层先列出分类，进分类才看到字段。50+ 项平铺在一个
// 列表里翻不动，分类后每屏 3~14 项。字段表按分类**连续排列**，分类内顺序即
// 表内顺序；加新项只要插到对应分类段里并给上 cat。
enum SettingsCat {
    CAT_GEN = 0,     // 通用：应用模式/开机视图/屏幕方向/休眠/待机表盘/蓝牙
    CAT_DISPLAY,     // 显示与版式：字体/文字方向/竖排参考线/排版
    CAT_IME,         // 输入法：模糊音/联想/候选/词库
    CAT_TYPEWRITER,  // 打字机模式：输入模式开关 + 按键音效
    CAT_EDIT,        // 编辑与保存：自动保存/草稿/版本/润色/个人资料
    CAT_NET,         // 网络与同步：WiFi/时间/WebDAV/Flomo/OPDS/文件管理
    CAT_AI,          // AI：Deepseek（语音听写已整块移除，本机无麦克风）
    // 「资源下载」分类已整类搬到阅读模式的设置标签（screen_reader.cpp 的 ResDl），
    // 下载出来的词典/字体都是给阅读用的，不再在写作设置里出现。
    CAT_COUNT,
};
static const char *SETTINGS_CAT_NAMES[CAT_COUNT] = {
    "通用", "显示与版式", "输入法", "打字机模式",
    "编辑与保存", "网络与同步", "AI",
};

struct SettingField { const char *key; const char *label; bool masked; bool action; int cat; };
static const SettingField SETTINGS_FIELDS[] = {
    // ── 通用 ──
    {"_app_mode", "工作模式", false, true, CAT_GEN},
    {"_home_view", "主页视图", false, true, CAT_GEN},
    {"_orientation", "屏幕方向", false, true, CAT_GEN},
    // 「自动休眠」已删：它和待机是同一件事，现在统一由阅读模式设置里的
    // 「自动待机」（关/5/10/15/20 分钟）一个选择器管，见 g_settings.autoStandbyMinutes()。
    {"sleep_screen", "休眠保留画面", false, false, CAT_GEN},
    {"_clock_face", "待机表盘", false, true, CAT_GEN},
    {"_clock_preview", "预览待机表盘", false, true, CAT_GEN},
    {"_bt_manage", "蓝牙设备管理", false, true, CAT_GEN},
    // 配置与阅读记录备份/恢复（settings_backup.cpp）。放「通用」这一类的末尾：它管的
    // 东西跨模式（写作的设置键 + 阅读的位置/笔记/统计 + 编辑器的版本历史），不属于
    // 任何单一分类。恢复之后要重启才生效 —— 文件换掉了，内存里那份设置缓存和各模块
    // 从设置派生的状态却还停在旧值上，重启是唯一不用逐模块追的地方。
    {"_cfg_backup", "备份设置与记录", false, true, CAT_GEN},
    {"_cfg_restore", "从备份恢复", false, true, CAT_GEN},
    // ── 显示与版式 ──
    {"_font", "字体", false, true, CAT_DISPLAY},
    {"night_mode", "夜间模式", false, false, CAT_DISPLAY},
    // 「阅读器方向」不在这一屏：方向是各模式自己的事，阅读模式在自己的设置标签里
    // 就有「阅读器方向」（screen_reader 的 Settings）；计划模式和写作模式各自一项。
    {"_gtd_orientation", "计划模式方向", false, true, CAT_DISPLAY},
    // 写作模式的**屏幕**方向。注意别和下面那个「文字方向」（_editor_orientation =
    // 正文横排/竖排）看混：这条管整屏转不转，那条管正文的书写方向。
    // 空串 = 跟随通用里的「屏幕方向」，进/出写作模式时套用/还原（screen_editor）。
    {"_writing_orientation", "写作模式方向", false, true, CAT_DISPLAY},
    // 错相揭页：阅读器翻页时用 16 条带依次入相的"揭页"替代普通差分刷（约 1.1s）。
    // 开着更好看但更慢，所以给个开关，默认开。
    {"page_turn_anim", "翻页动画", false, false, CAT_DISPLAY},
    {"reader_perf_log", "翻页性能日志", false, false, CAT_DISPLAY},
    {"ui_perf_log", "界面性能日志", false, false, CAT_DISPLAY},
    {"webdav_debug_log", "WebDAV调试日志", false, false, CAT_DISPLAY},
    {"_editor_orientation", "文字方向", false, true, CAT_DISPLAY},
    {"vertical_ref_line", "竖排参考线", false, false, CAT_DISPLAY},
    {"_vertical_ref_line_style", "参考线样式", false, true, CAT_DISPLAY},
    {"md_render", "Markdown渲染", false, false, CAT_DISPLAY},
    {"first_line_indent", "首行缩进", false, false, CAT_DISPLAY},
    // 编辑区正文字号：**只管编辑器正文那一块**（换行/光标/选区/触摸命中全按它算），
    // 状态栏、输入法条、查找/帮助浮层仍是界面字号。见 ui_helpers.h 的 editorBodyFontPx。
    {"_editor_font_size", "正文字号", false, true, CAT_DISPLAY},
    // ── 输入法 ──
    {"_kb_layout", "键盘布局", false, true, CAT_IME},
    {"_ime_fuzzy", "拼音模糊音", false, true, CAT_IME},
    {"_ime_predict_mode", "联想候选", false, true, CAT_IME},
    // 候选字单独一档字号（像素高）。45=标准，与界面 20pt 同高；调大候选项会顶高
    // 键盘面板，正文可视行数相应减少（几何见 editor_vk.cpp 的 evkCandRowH）。
    {"_ime_cand_size", "候选字大小", false, true, CAT_IME},
    {"ime_candidate_highlight", "候选高亮", false, false, CAT_IME},
    // 编码区+候选区那两行的清残影时机。清一次 = 一次区域 GC16（≈330ms，与区域大小
    // 无关，见 ui_render.cpp），默认只挑句读：那是用户天然停手组织下一句的时刻。
    {"_ime_clean", "清残影时机", false, true, CAT_IME},
    // 实体键盘打字时正文那一拍怎么刷：稳 = 整屏阈值 DU（墨实，每键约 220ms）；快 =
    // 差分矩形跟随 DU（约 56ms，字先发灰，停顿那次 GC16 坐实）。默认稳。
    {"_ime_commit_mode", "上屏刷法", false, true, CAT_IME},
    {"ime_sentence", "整句候选", false, false, CAT_IME},
    {"ime_doc_context", "正文词优先", false, false, CAT_IME},
    {"_dict_mgr", "词库管理", false, true, CAT_IME},
    // ── 打字机模式 ──
    // 输入模式开关和它的按键音效设置放一起：以前平铺时它们紧挨着，拆开分类后
    // 「打字机模式怎么不见了」——模式开关本身就是这一类的入口。
    {"_input_mode", "输入模式", false, true, CAT_TYPEWRITER},
    {"click_enabled", "按键音效", false, false, CAT_TYPEWRITER},
    {"_click_chinese", "中文音效触发", false, true, CAT_TYPEWRITER},
    {"_click_volume", "按键音效音量", false, true, CAT_TYPEWRITER},
    // 只有一种反馈音(见 typing_click.cpp)，所以没有音色/亮度可调；留一个试听，
    // 免得为了听一声还得退出去打字。
    {"_click_try", "按键音效试听", false, true, CAT_TYPEWRITER},
    // ── 编辑与保存 ──
    {"auto_save", "自动保存", false, false, CAT_EDIT},
    {"recovery_draft", "恢复草稿", false, false, CAT_EDIT},
    {"version_history", "版本历史", false, false, CAT_EDIT},
    {"_polish_prompt", "润色提示词", false, true, CAT_EDIT},
    {"personal_exp", "个人经历", false, false, CAT_EDIT},
    {"personal_hob", "个人爱好", false, false, CAT_EDIT},
    // ── 网络与同步 ──
    {"wifi_ssid", "WiFi SSID", false, false, CAT_NET},
    {"wifi_pass", "WiFi 密码", false, false, CAT_NET},
    {"_sync_time", "网络同步时间", false, true, CAT_NET},
    {"timezone", "时区(如CST-8)", false, false, CAT_NET},
    {"ntp_server", "NTP服务器", false, false, CAT_NET},
    {"webdav_url", "WebDAV URL", false, false, CAT_NET},
    {"webdav_user", "WebDAV 用户", false, false, CAT_NET},
    {"webdav_pass", "WebDAV 密码", false, false, CAT_NET},
    {"flomo_email", "Flomo 邮箱", false, false, CAT_NET},
    {"flomo_pass", "Flomo 密码", false, false, CAT_NET},
    {"_flomo_token", "生成Flomo Token", false, true, CAT_NET},
    {"opds_url", "OPDS 目录地址", false, false, CAT_NET},
    {"_file_mgr", "文件管理", false, true, CAT_NET},
    {"file_mgr_token", "文件管理密码", true, false, CAT_NET},
    // ── AI ──（原来的「语音识别服务/百度 Api·Secret Key」随语音听写一起去掉了：
    // 本机没有麦克风，VoiceInput 是桩，留着这些输入框只会让人以为能录音。）
    {"deepseek_key", "Deepseek Key", false, false, CAT_AI},
};
static const int NUM_SETTINGS = sizeof(SETTINGS_FIELDS) / sizeof(SETTINGS_FIELDS[0]);

// 打字机专用设置行仅在该模式开启时显示
static bool fieldHidden(int idx) {
    const char *k = SETTINGS_FIELDS[idx].key;
    if (strcmp(k, "vertical_ref_line") == 0)
        return g_settings.editorOrientation() != "vertical";
    if (strcmp(k, "_vertical_ref_line_style") == 0)
        return g_settings.editorOrientation() != "vertical" || !g_settings.verticalReferenceLine();
    // 按键音效这一行**任何模式**下都显示：它是虚拟键盘的按键反馈，不只是打字机模式的
    // 配菜。下面几个参数行跟着开关走——关了就没有可调的。
    if (strcmp(k, "_click_volume") == 0 || strcmp(k, "_click_try") == 0 ||
        strcmp(k, "_click_chinese") == 0)
        return !g_settings.typingClickEnabled();
    return false;
}

// 下拉选项的 key↔中文名小表(key 是英文标识，label 显示用)。
struct OptItem { const char *key; const char *label; };
// 中文音效触发 key↔中文名(顺序即循环顺序)
static const OptItem CLICK_CHINESE_OPTS[] = {
    {"key", "按键触发"}, {"count", "上屏触发(按字数)"}, {"single", "上屏触发(单声)"},
};
static int clickChineseIndex(const char *k) {
    for (int i = 0; i < (int)(sizeof(CLICK_CHINESE_OPTS) / sizeof(CLICK_CHINESE_OPTS[0])); i++)
        if (strcmp(k, CLICK_CHINESE_OPTS[i].key) == 0) return i;
    return 0;
}
// 竖排参考线样式 key↔中文名
static const OptItem VERTICAL_REF_LINE_STYLE_OPTS[] = {
    {"solid", "实线"}, {"dash", "虚线"}, {"dot", "点状虚线"},
};
static int verticalRefLineStyleIndex(const char *k) {
    for (int i = 0; i < (int)(sizeof(VERTICAL_REF_LINE_STYLE_OPTS) / sizeof(VERTICAL_REF_LINE_STYLE_OPTS[0])); i++)
        if (strcmp(k, VERTICAL_REF_LINE_STYLE_OPTS[i].key) == 0) return i;
    return 0;
}
// 拼音模糊音设置: key 是 settings 文件值,label 是设置页显示
static const OptItem IME_FUZZY_OPTS[] = {
    {"zcs", "z/zh c/ch s/sh"},
    {"zcs,nl", "+ n/l"},
    {"zcs,eneng,ining", "+ 前后鼻音"},
    {"all", "全部"},
    {"off", "关闭"},
};
static int imeFuzzyIndex(const char *k) {
    for (int i = 0; i < (int)(sizeof(IME_FUZZY_OPTS) / sizeof(IME_FUZZY_OPTS[0])); i++)
        if (strcmp(k, IME_FUZZY_OPTS[i].key) == 0) return i;
    return 0;
}
static const OptItem IME_PREDICT_OPTS[] = {
    {"always", "总是"}, {"space", "空格后"}, {"off", "关闭"},
};
static int imePredictIndex(const char *k) {
    for (int i = 0; i < (int)(sizeof(IME_PREDICT_OPTS) / sizeof(IME_PREDICT_OPTS[0])); i++)
        if (strcmp(k, IME_PREDICT_OPTS[i].key) == 0) return i;
    return 0;
}
// 输入法候选字大小：key = 候选字像素高，标准 45 与界面 20pt(line_height) 同高。
// 这个设置**同时**管两个候选行：虚拟键盘的候选条与实体键盘的输入法条，两处都按
// ui_helpers.cpp 的 imeCandFontPx 直写像素（行高各自另算：键盘面板 evkCandRowH，
// 输入法条 imeBarRowH）。
static const OptItem IME_CAND_SIZE_OPTS[] = {
    {"34", "小"}, {"45", "标准"}, {"56", "大"}, {"68", "特大"},
};
static int imeCandSizeIndex(const char *k) {
    for (int i = 0; i < (int)(sizeof(IME_CAND_SIZE_OPTS) / sizeof(IME_CAND_SIZE_OPTS[0])); i++)
        if (strcmp(k, IME_CAND_SIZE_OPTS[i].key) == 0) return i;
    return 1;  // 认不出 → 标准档
}
// 编辑区正文字号：key = 正文的光栅像素高，标准 45 与界面 20pt(line_height) 同高，
// 也就是这个设置**没动过**时的逐像素原样。只管编辑器正文那一块（换行/光标/选区/触摸
// 命中全按它算），状态栏、输入法条、虚拟键盘、各浮层仍是界面字号——见 ui_helpers.h 的
// editorBodyFontPx 与 screen_editor_handle 顶部的说明。
static const OptItem EDITOR_FONT_SIZE_OPTS[] = {
    {"34", "小"}, {"45", "标准"}, {"56", "大"}, {"68", "特大"},
};
static int editorFontSizeIndex(const char *k) {
    for (int i = 0; i < (int)(sizeof(EDITOR_FONT_SIZE_OPTS) / sizeof(EDITOR_FONT_SIZE_OPTS[0])); i++)
        if (strcmp(k, EDITOR_FONT_SIZE_OPTS[i].key) == 0) return i;
    return 1;  // 认不出 → 标准档
}
// 虚拟键盘键位布局：26 键全拼 / 14 / 18 / 9 键（后三个是"一个键多个字母"的歧义布局，
// 分组表取自万象拼音，见 IME.cpp 顶部）。存的就是 "26"/"14"/"18"/"9"，与
// editor_vk 的 editorVkSetLayout 同一套字面量。
static const OptItem KB_LAYOUT_OPTS[] = {
    {"26", "26键全拼"}, {"14", "14键"}, {"18", "18键"}, {"9", "9键"},
};
// 输入法那两行（编码区+候选区）什么时候清残影。清一次 = 一次区域 GC16，约 330ms
// （耗时看相位数，不看区域大小）；上屏与句读都记的话，一句话里会清两遍（用户报障
// "会刷新2次"），所以默认只留句读。见 ui_render.cpp 的 ime_clean_policy。
// "never"是最激进的一档：输入法这条路**一次区域 GC16 都不做**（连快档那条跟设置无关的
// 计数兜底也一起关掉），打字全程只快刷 —— 代价是发灰的正文与键盘残影只能等下一次整屏
// 全刷（翻页/换章/摇一摇）才被扫掉。**它跟 "off" 不是一回事**：off 只关"句读/上屏那两拍
// 记账"，快档自己欠下的那笔照还。
static const OptItem IME_CLEAN_OPTS[] = {
    {"punct", "句读后"}, {"commit", "上屏后"}, {"both", "两者都清"}, {"off", "不清"},
    {"never", "从不清(只快刷)"},
};
static int imeCleanIndex(const char *k) {
    for (int i = 0; i < (int)(sizeof(IME_CLEAN_OPTS) / sizeof(IME_CLEAN_OPTS[0])); i++)
        if (strcmp(k, IME_CLEAN_OPTS[i].key) == 0) return i;
    return 0;  // 认不出 → 句读后（与 settings_manager 的默认值一致）
}
// 实体键盘打字时正文那一拍的推屏方式（见 ui_render.cpp 的 render_present 第 2 条路）：
//   solid = 整屏阈值 DU（每键约 220ms，墨实，现在就是这样）
//   fast  = 只推差分矩形的跟随 DU（约 56ms，但跟随表推力只有阈值表的 1/4，刚上屏的
//           字先发灰，等打字停顿那次区域 GC16 坐实）。默认 solid = 保持现状。
static const OptItem IME_COMMIT_OPTS[] = {
    {"solid", "稳（整屏）"}, {"fast", "快（差分）"},
};
static int imeCommitIndex(const char *k) {
    for (int i = 0; i < (int)(sizeof(IME_COMMIT_OPTS) / sizeof(IME_COMMIT_OPTS[0])); i++)
        if (strcmp(k, IME_COMMIT_OPTS[i].key) == 0) return i;
    return 0;  // 认不出 → 稳（与 settings_manager 的默认值一致）
}
static int kbLayoutIndex(const char *k) {
    for (int i = 0; i < (int)(sizeof(KB_LAYOUT_OPTS) / sizeof(KB_LAYOUT_OPTS[0])); i++)
        if (strcmp(k, KB_LAYOUT_OPTS[i].key) == 0) return i;
    return 0;
}
// UI 序号(跳过隐藏行)→ SETTINGS_FIELDS 真实下标;越界返回最后一个可见行
// 当前所在分类(顶层分类列表/子菜单共用)。定义在字段过滤之前，供 fieldAt 等使用。
static int s_cat = CAT_GEN;
// 「从备份恢复」的二次确认。恢复是不可逆的覆盖，第一次点只是把这句话打出来，
// 离开这一屏（screen_settings_init）就作废 —— 免得下一次进来第一下就恢复。
static bool s_cfgRestoreConfirm = false;

// 该字段当前是否出现在列表里：属于当前分类，且未被模式条件隐藏。
static bool fieldShown(int idx) {
    return SETTINGS_FIELDS[idx].cat == s_cat && !fieldHidden(idx);
}

// 分类里的可见项数(分类列表用来显示 "n 项")。
static int catFieldCount(int cat) {
    int n = 0;
    for (int i = 0; i < NUM_SETTINGS; i++)
        if (SETTINGS_FIELDS[i].cat == cat && !fieldHidden(i)) n++;
    return n;
}

static int fieldAt(int sel) {
    int lastVisible = -1, vis = 0;
    for (int i = 0; i < NUM_SETTINGS; i++) {
        if (!fieldShown(i)) continue;
        if (vis == sel) return i;
        lastVisible = i;
        vis++;
    }
    return lastVisible >= 0 ? lastVisible : 0;
}
static int fieldVisibleCount() {
    int n = 0;
    for (int i = 0; i < NUM_SETTINGS; i++)
        if (fieldShown(i)) n++;
    return n;
}

// ── 选项弹层：轮换制设置项 ────────────────────────────────────────────────
// 这些项以前按回车是"循环到下一个值"：想往回选得绕一整圈，选项多了根本记不住有哪些
// 可选。现在一律弹层点选（↑↓/点按移动，回车落定，Esc 取消）。
struct PickerOpt { std::string value; std::string label; };

// 表驱动选项（OptItem 表）→ 弹层选项：顺序即显示顺序，key 即落进 settings 的值。
static std::vector<PickerOpt> optsFromTable(const OptItem *t, int n) {
    std::vector<PickerOpt> v;
    v.reserve(n);
    for (int i = 0; i < n; i++) v.push_back({t[i].key, t[i].label});
    return v;
}
#define OPT_ITEM_N(t) ((int)(sizeof(t) / sizeof((t)[0])))

// 该设置项是否改用弹层选择。与 pickerOpts() 是同一批 key——只有"取值有穷"的项才做；
// 那些直接执行动作/进子界面的（词库管理、文件管理、同步时间…）不在内。
static bool pickerFieldSupported(const char *key) {
    return strcmp(key, "_app_mode") == 0 || strcmp(key, "_home_view") == 0 ||
           strcmp(key, "_orientation") == 0 || strcmp(key, "_gtd_orientation") == 0 ||
           strcmp(key, "_writing_orientation") == 0 ||
           strcmp(key, "_editor_orientation") == 0 || strcmp(key, "_clock_face") == 0 ||
           strcmp(key, "_font") == 0 || strcmp(key, "_input_mode") == 0 ||
           strcmp(key, "_kb_layout") == 0 || strcmp(key, "_ime_fuzzy") == 0 ||
           strcmp(key, "_ime_predict_mode") == 0 || strcmp(key, "_ime_cand_size") == 0 ||
           strcmp(key, "_ime_clean") == 0 || strcmp(key, "_ime_commit_mode") == 0 ||
           strcmp(key, "_editor_font_size") == 0 ||
           strcmp(key, "_click_volume") == 0 ||
           strcmp(key, "_click_chinese") == 0 || strcmp(key, "_vertical_ref_line_style") == 0;
}

// 该设置项可选的取值。value 落进 settings，label 显示。字体项要扫 SD（有 I/O 开销），
// 所以只在**打开弹层那一下**调一次，结果缓存进 g_settingsState.pickerOpts。
static std::vector<PickerOpt> pickerOpts(const char *key) {
    if (strcmp(key, "_app_mode") == 0) return {{"quick", "快捷编辑"}, {"journal", "个人日记"}};
    if (strcmp(key, "_home_view") == 0) return {{"month", "月视图"}, {"week", "周视图"}};
    if (strcmp(key, "_orientation") == 0) return {{"portrait", "纵向"}, {"landscape", "横向"}};
    // 「自适应」= 按加速度计的重力方向自己认横竖（见 hw/auto_orient.h）。**只在进入
    // 这个模式之后跟着转**，模式外面不动 —— 它是各模式自己的方向设置，不是全局的。
    if (strcmp(key, "_gtd_orientation") == 0)
        return {{"", "跟随屏幕方向"}, {"portrait", "纵向"}, {"landscape", "横向"}, {"auto", "自适应"}};
    if (strcmp(key, "_writing_orientation") == 0)
        return {{"", "跟随屏幕方向"}, {"portrait", "纵向"}, {"landscape", "横向"}, {"auto", "自适应"}};
    if (strcmp(key, "_editor_orientation") == 0) return {{"horizontal", "横排"}, {"vertical", "竖排"}};
    if (strcmp(key, "_clock_face") == 0) {
        std::vector<PickerOpt> v;
        for (StandbyFace f : {StandbyFace::Off, StandbyFace::Clock, StandbyFace::Almanac, StandbyFace::Cover,
                              StandbyFace::Image})
            v.push_back({standbyFaceKey(f), standbyFaceLabel(f)});
        return v;
    }
    if (strcmp(key, "_font") == 0) {
        std::vector<PickerOpt> v;
        v.push_back({"", "内建"});
        const int n = ttf_font_scan();
        for (int i = 0; i < n; i++) {
            const ttf_font_item_t *it = ttf_font_item(i);
            if (it) v.push_back({it->path, it->name});
        }
        return v;
    }
    if (strcmp(key, "_input_mode") == 0) return {{"normal", "正常模式"}, {"typewriter", "打字机模式"}};
    if (strcmp(key, "_kb_layout") == 0) return optsFromTable(KB_LAYOUT_OPTS, OPT_ITEM_N(KB_LAYOUT_OPTS));
    if (strcmp(key, "_ime_fuzzy") == 0) return optsFromTable(IME_FUZZY_OPTS, OPT_ITEM_N(IME_FUZZY_OPTS));
    if (strcmp(key, "_ime_predict_mode") == 0)
        return optsFromTable(IME_PREDICT_OPTS, OPT_ITEM_N(IME_PREDICT_OPTS));
    if (strcmp(key, "_ime_cand_size") == 0)
        return optsFromTable(IME_CAND_SIZE_OPTS, OPT_ITEM_N(IME_CAND_SIZE_OPTS));
    if (strcmp(key, "_ime_clean") == 0) return optsFromTable(IME_CLEAN_OPTS, OPT_ITEM_N(IME_CLEAN_OPTS));
    if (strcmp(key, "_ime_commit_mode") == 0)
        return optsFromTable(IME_COMMIT_OPTS, OPT_ITEM_N(IME_COMMIT_OPTS));
    if (strcmp(key, "_editor_font_size") == 0)
        return optsFromTable(EDITOR_FONT_SIZE_OPTS, OPT_ITEM_N(EDITOR_FONT_SIZE_OPTS));
    if (strcmp(key, "_click_chinese") == 0)
        return optsFromTable(CLICK_CHINESE_OPTS, OPT_ITEM_N(CLICK_CHINESE_OPTS));
    if (strcmp(key, "_vertical_ref_line_style") == 0)
        return optsFromTable(VERTICAL_REF_LINE_STYLE_OPTS, OPT_ITEM_N(VERTICAL_REF_LINE_STYLE_OPTS));
    if (strcmp(key, "_click_volume") == 0) {
        std::vector<PickerOpt> v;
        for (int lv : {0, 20, 40, 60, 80, 100}) {
            std::string s = std::to_string(lv);
            v.push_back({s, s + "%"});
        }
        return v;
    }
    return {};
}

// 当前值（与 pickerOpts 的 value 同一套字面量，用来把弹层的初始选中项落在现用值上）。
static std::string pickerCurValue(const char *key) {
    if (strcmp(key, "_app_mode") == 0) return g_settings.appMode();
    if (strcmp(key, "_home_view") == 0) return g_settings.homeView();
    if (strcmp(key, "_orientation") == 0) return g_settings.orientation();
    if (strcmp(key, "_gtd_orientation") == 0) return g_settings.getString("gtd_orientation", "");
    if (strcmp(key, "_writing_orientation") == 0)
        return g_settings.getString("writing_orientation", "");
    if (strcmp(key, "_editor_orientation") == 0) return g_settings.editorOrientation();
    if (strcmp(key, "_clock_face") == 0)
        return standbyFaceKey(standbyFaceFromKey(g_settings.getString("clock_face").c_str()));
    if (strcmp(key, "_font") == 0) {
        const char *cur = font_store_get_path();
        return (cur && !ttf_font_path_is_builtin(cur)) ? std::string(cur) : std::string();
    }
    if (strcmp(key, "_input_mode") == 0) return g_settings.inputMode();
    if (strcmp(key, "_kb_layout") == 0) return g_settings.getString("kb_layout");
    if (strcmp(key, "_ime_fuzzy") == 0) return g_settings.imeFuzzy();
    if (strcmp(key, "_ime_predict_mode") == 0) return g_settings.imePredictMode();
    if (strcmp(key, "_ime_cand_size") == 0)
        return IME_CAND_SIZE_OPTS[imeCandSizeIndex(g_settings.getString("ime_cand_size", "45").c_str())].key;
    if (strcmp(key, "_ime_clean") == 0)
        return IME_CLEAN_OPTS[imeCleanIndex(g_settings.imeCleanMode().c_str())].key;
    if (strcmp(key, "_ime_commit_mode") == 0)
        return IME_COMMIT_OPTS[imeCommitIndex(g_settings.imeCommitMode().c_str())].key;
    if (strcmp(key, "_editor_font_size") == 0)
        return EDITOR_FONT_SIZE_OPTS[editorFontSizeIndex(g_settings.getString("editor_font_size", "45").c_str())].key;
    if (strcmp(key, "_click_volume") == 0) return std::to_string(g_settings.typingClickVolume());
    if (strcmp(key, "_click_chinese") == 0) return g_settings.clickChineseMode();
    if (strcmp(key, "_vertical_ref_line_style") == 0) return g_settings.verticalReferenceLineStyle();
    return "";
}

// 布尔型开关项:显示 开/关,Enter 在 "0"/"1" 间切换
static bool isToggleField(const char *key) {
    return strcmp(key, "auto_save") == 0 ||
           strcmp(key, "sleep_screen") == 0 || strcmp(key, "md_render") == 0 ||
           strcmp(key, "first_line_indent") == 0 || strcmp(key, "version_history") == 0 ||
           strcmp(key, "recovery_draft") == 0 || strcmp(key, "vertical_ref_line") == 0 ||
           strcmp(key, "click_enabled") == 0 ||
           strcmp(key, "ime_sentence") == 0 ||
           strcmp(key, "ime_doc_context") == 0 || strcmp(key, "ime_candidate_highlight") == 0 ||
           strcmp(key, "page_turn_anim") == 0 ||
           strcmp(key, "reader_perf_log") == 0 ||
           strcmp(key, "ui_perf_log") == 0 ||
           strcmp(key, "webdav_debug_log") == 0 ||
           strcmp(key, "night_mode") == 0;
}

static bool toggleValue(const char *key) {
    std::string v = g_settings.getString(key);
    if (strcmp(key, "md_render") == 0) return v != "0";   // 默认开
    if (strcmp(key, "recovery_draft") == 0) return v != "0";  // 默认开
    // 不走 v != "0"：没设置过时默认值跟模式走(打字机模式下默认开)，显示必须和实际
    // 出声与否一致，否则会看到"关着却在响"。
    if (strcmp(key, "click_enabled") == 0) return g_settings.typingClickEnabled();
    if (strcmp(key, "ime_sentence") == 0) return v != "0";  // 默认开
    if (strcmp(key, "ime_doc_context") == 0) return v != "0";  // 默认开
    if (strcmp(key, "ime_candidate_highlight") == 0) return v == "1";  // 默认关
    if (strcmp(key, "page_turn_anim") == 0) return v != "0";           // 默认开
    // 走访问器：它带"读不到新键就回退旧键 reader_night"的迁移逻辑。
    if (strcmp(key, "night_mode") == 0) return g_settings.nightMode();
    return v == "1";  // auto_save: 默认关
}

static bool restartRequiredField(const char *key) {
    return strcmp(key, "_ime_fuzzy") == 0 ||
           strcmp(key, "ime_sentence") == 0 ||
           strcmp(key, "ime_doc_context") == 0 ||
           strcmp(key, "ime_candidate_highlight") == 0;
}

// SETTINGS_CATEGORY = 分类列表(顶层)；SETTINGS_BROWSE = 某个分类下的字段列表。
enum SettingsScreenMode {
    SETTINGS_CATEGORY, SETTINGS_BROWSE, SETTINGS_DICT_CHOOSE, SETTINGS_DICT_LIST, SETTINGS_DICT_ADD
};

static struct {
    int selection = 0;
    int scroll = 0;
    bool editing = false;
    std::string editBuffer;
    int editCursor = 0;
    bool imeActive = false;
    SettingsScreenMode mode = SETTINGS_CATEGORY;
    IME::UserDictKind dictKind = IME::FIXED_DICT;
    int dictSelection = 0;
    int dictScroll = 0;
    std::set<int> dictSelected;
    std::string dictAddBuffer;
    int dictAddCursor = 0;
    bool dictAddImeActive = false;
    bool dictSearching = false;
    std::string dictSearchBuffer;
    int dictSearchCursor = 0;
    bool dictSearchImeActive = false;
    std::string dictNotice;
    bool dictCacheValid = false;
    bool dictFilterCacheValid = false;
    IME::UserDictKind dictCacheKind = IME::FIXED_DICT;
    std::string dictFilterCacheQuery;
    std::vector<IME::UserEntryView> dictEntriesCache;
    std::vector<int> dictFilteredCache;
    // 轮换制设置项的选项弹层（见 pickerFieldSupported）。打开时列表照画、浮层盖在上面。
    bool pickerOpen = false;
    int pickerField = -1;              // SETTINGS_FIELDS 下标（不是可见行号）
    int pickerSel = 0;
    int pickerScroll = 0;
    std::vector<PickerOpt> pickerItems;  // 打开时算一次并缓存（字体项要扫 SD，不能每帧扫）
} g_settingsState;

static ImeField settingsEditField()       { return ImeField{&g_settingsState.editBuffer, &g_settingsState.editCursor}; }
static ImeField settingsDictSearchField() { return ImeField{&g_settingsState.dictSearchBuffer, &g_settingsState.dictSearchCursor}; }
static ImeField settingsDictAddField()    { return ImeField{&g_settingsState.dictAddBuffer, &g_settingsState.dictAddCursor}; }

static const char *dictKindLabel(IME::UserDictKind kind) {
    if (kind == IME::FIXED_DICT) return "固定词库";
    if (kind == IME::PREDICT_DICT) return "联想词库";
    return "动态词库";
}

static int dictKindLimit(IME::UserDictKind kind) {
    if (kind == IME::DYNAMIC_DICT) return 5000;
    if (kind == IME::PREDICT_DICT) return 2000;
    return 1000;
}

static const char *dictExportPath(IME::UserDictKind kind) {
    if (kind == IME::FIXED_DICT) return "/sdcard/settings/userdict_fixed_export.txt";
    if (kind == IME::PREDICT_DICT) return "/sdcard/settings/userpredict_export.txt";
    return "/sdcard/settings/userdict_export.txt";
}

static const char *dictImportPath(IME::UserDictKind kind) {
    if (kind == IME::FIXED_DICT) return "/sdcard/settings/userdict_fixed_import.txt";
    if (kind == IME::PREDICT_DICT) return "/sdcard/settings/userpredict_import.txt";
    return "/sdcard/settings/userdict_import.txt";
}

static const char *dictImportErrorPath(IME::UserDictKind kind) {
    if (kind == IME::FIXED_DICT) return "/sdcard/settings/userdict_fixed_import_errors.txt";
    if (kind == IME::PREDICT_DICT) return "/sdcard/settings/userpredict_import_errors.txt";
    return "/sdcard/settings/userdict_import_errors.txt";
}

static IME::UserDictKind dictKindFromSelection(int sel) {
    if (sel == 0) return IME::FIXED_DICT;
    if (sel == 1) return IME::DYNAMIC_DICT;
    return IME::PREDICT_DICT;
}

// ── 轮换制设置项：弹层选择 ────────────────────────────────────────────────
// 落定一个值，并做该项各自需要的即时副作用。这些副作用原来写在"回车循环到下一个值"
// 的分支里，入口换成弹层后原样搬过来——同一个动作不能留两份实现。
static void drawBrowseList();   // 定义在文件末尾（screen_settings_handle 之后）
// 只画不提交的那一半：弹层的底图要用它。**不能直接用 drawBrowseList()**——那个自己
// 带 ui_commit()，弹层再画一层框又提交一次，等于每帧提交"只有列表"和"列表+浮层"两张
// 不同的画面，面板就在两者之间来回刷（实测：选择框一直闪）。
static void drawBrowseListBody();

static void pickerApply(const char *key, const std::string &value, ScreenContext &ctx) {
    if (strcmp(key, "_app_mode") == 0) {
        g_settings.setString("app_mode", value);
        ctx.statusMessage = "切换模式需重启生效";
    } else if (strcmp(key, "_home_view") == 0) {
        g_settings.setString("home_view", value);
    } else if (strcmp(key, "_orientation") == 0) {
        // 屏幕方向切换立即生效：改旋转 + 快照失效 → 下一帧 ui_clear/重绘/整屏 GC16。
        g_settings.setOrientation(value);
        board_apply_orientation(value.c_str());
        ui_invalidate_snapshot();
    } else if (strcmp(key, "_gtd_orientation") == 0) {
        // 只写键，**不调 board_force_***：在主界面里翻转屏幕会把设置界面自己也转过去。
        // 计划模式进/出时自己套用/还原（screen_gtd 的 applyGtdOrientation）。
        g_settings.setString("gtd_orientation", value);
    } else if (strcmp(key, "_writing_orientation") == 0) {
        // 同上：写作模式进/出时自己套用/还原（screen_editor 的 applyEditorOrientation）。
        g_settings.setString("writing_orientation", value);
    } else if (strcmp(key, "_editor_orientation") == 0) {
        g_settings.setString("editor_orientation", value);
        // 竖排才显示的两行会跟着出现/消失，选中行可能落到列表外。
        if (g_settingsState.selection > fieldVisibleCount() - 1)
            g_settingsState.selection = fieldVisibleCount() - 1;
    } else if (strcmp(key, "_clock_face") == 0) {
        g_settings.setString("clock_face", value);
    } else if (strcmp(key, "_font") == 0) {
        // 只重算/清空**内容面**：g_font 是 UI 实例（恒内置），在它上面 reloadFont 会把
        // UI 的 ascender 从错的面上重算、清错缓存，意图完全丢失。
        font_store_set_path(value.c_str());
        ttf_font_open(value.c_str());
        g_content_font.reloadFont();
    } else if (strcmp(key, "_input_mode") == 0) {
        g_settings.setString("input_mode", value);
        // 这里不再 typingClickRelease()：正常模式现在也能开按键音(当虚拟键盘反馈)，
        // 切模式不该顺带把喇叭掐了。真正该停的是把开关关掉那一下。
        if (g_settingsState.selection > fieldVisibleCount() - 1)
            g_settingsState.selection = fieldVisibleCount() - 1;
    } else if (strcmp(key, "_kb_layout") == 0) {
        // 换布局 = 换整张键位表。写完立刻生效，下次弹出键盘就是新布局。
        editorVkSetLayout(value.c_str());
    } else if (strcmp(key, "_ime_fuzzy") == 0) {
        g_settings.setString("ime_fuzzy", value);
    } else if (strcmp(key, "_ime_predict_mode") == 0) {
        g_settings.setString("ime_predict_mode", value);
    } else if (strcmp(key, "_ime_clean") == 0) {
        // 只写键：ui_render 在 core0 组装每一帧的 UiJob 时现读（见 ime_clean_policy），
        // 下一拍就生效，不用通知任何人。
        g_settings.setString("ime_clean", value);
    } else if (strcmp(key, "_ime_commit_mode") == 0) {
        // 只写键：ui_render 在 core0 组装每一帧的 UiJob 时现读（见 ime_commit_fast_policy），
        // 下一拍就生效，不用通知任何人。
        g_settings.setString("ime_commit_mode", value);
    } else if (strcmp(key, "_ime_cand_size") == 0) {
        // 只写键：候选行几何每次绘制现算（ui_helpers 的 imeCandFontPx），下次重绘就是新字号。
        g_settings.setString("ime_cand_size", value);
        // 候选字宽是按字符串缓存的，档位一换就得作废，否则分页还按旧字号算。
        IME::getInstance().invalidateCandidateWidths();
    } else if (strcmp(key, "_editor_font_size") == 0) {
        // 只写键：正文几何每次绘制现算（ui_helpers 的 editorBodyFontPx 走 FontScope）。
        // 编辑器的折行缓存按字号存了一份（screen_editor 的 cachedBodyPx），字号一改
        // 下次 getVrows 自己会重排，这里不用通知谁。
        g_settings.setString("editor_font_size", value);
    } else if (strcmp(key, "_click_volume") == 0) {
        g_settings.setString("click_volume", value);
        typingClickAudition(3);   // 改完立刻听，不然得退出去打字才知道调没调对
    } else if (strcmp(key, "_click_chinese") == 0) {
        g_settings.setString("click_chinese", value);
    } else if (strcmp(key, "_vertical_ref_line_style") == 0) {
        g_settings.setString("vertical_ref_line_style", value);
    }
}

// 弹层框：标题行（设置项名）+ 分隔线 + 选项行。**不含推屏**——浮层要叠在列表底图上，
// 一次提交只能有一个绘制目标，commit 交给调用方。
static void pickerBoxRect(const std::vector<PickerOpt> &items, int *bx, int *by, int *bw, int *bh,
                          int *rows) {
    int maxRows = (SCREEN_H - 4 * FONT_H) / LINE_SPACING;   // 上下各留两行余量
    if (maxRows > 10) maxRows = 10;
    if (maxRows < 3) maxRows = 3;
    const int n = (int)items.size();
    int r = n < maxRows ? n : maxRows;
    if (r < 1) r = 1;
    *rows = r;

    int w = g_font.textWidth(SETTINGS_FIELDS[g_settingsState.pickerField].label) + 2 * FONT_H;
    for (const auto &it : items) {
        int tw = g_font.textWidth(it.label.c_str()) + 3 * FONT_H;
        if (tw > w) w = tw;
    }
    if (w < 240) w = 240;
    if (w > SCREEN_W - 24) w = SCREEN_W - 24;
    *bw = w;
    *bh = 8 + FONT_H + 6 + (*rows) * LINE_SPACING + 8;
    *bx = (SCREEN_W - w) / 2;
    *by = (SCREEN_H - *bh) / 2;
}

// 选项 i 的文字基线（绘制与命中共用）。
static int pickerRowY(int by, int i) {
    int sepY = by + 8 + FONT_H + 2;   // 标题行下沿的分隔线
    return sepY + 6 + g_font.ascent() + i * LINE_SPACING;
}

// 超宽文字截断加省略号：SD 字体名可能很长，而浮层框固定在屏内。
// **绝不能返回空串**：一个字都放不下时返回空串，这一行就成了"空白但能点"——用户看到
// 的就是"有的字体没有名字"，其实名字在、只是被裁没了。裁到一个字都放不下时就退成省略号。
static std::string fitText(const std::string &s, int maxW) {
    if (g_font.textWidth(s.c_str()) <= maxW) return s;
    std::string out = s;
    while (!out.empty()) {
        out.pop_back();
        while (!out.empty() && ((unsigned char)out.back() & 0xC0) == 0x80) out.pop_back();
        std::string probe = out + "…";
        if (g_font.textWidth(probe.c_str()) <= maxW) return probe;
    }
    return "…";
}

static void openPicker(int fieldIdx) {
    const SettingField &f = SETTINGS_FIELDS[fieldIdx];
    g_settingsState.pickerField = fieldIdx;
    g_settingsState.pickerItems = pickerOpts(f.key);
    g_settingsState.pickerOpen = true;
    g_settingsState.pickerScroll = 0;
    const std::string cur = pickerCurValue(f.key);
    int sel = 0;
    for (int i = 0; i < (int)g_settingsState.pickerItems.size(); i++) {
        if (g_settingsState.pickerItems[i].value == cur) { sel = i; break; }
    }
    g_settingsState.pickerSel = sel;
    // 字体选择器"有的行没名字"排查用：把每一项的 label 和**实际量出来的宽度**打出来。
    // label 空 = 扫描/font_stem 出了岔子；label 非空但宽度异常小 = 字形不在 UI 字面里
    // （画出来就是空白，但行照样可点）。两种都在这一行日志里分得清。
    if (strcmp(f.key, "_font") == 0) {
        int bx, by, bw, bh, rows;
        pickerBoxRect(g_settingsState.pickerItems, &bx, &by, &bw, &bh, &rows);
        ESP_LOGI(TAG, "字体选择器: %u 项, 框宽 %d, 每行可用 %d",
                 (unsigned)g_settingsState.pickerItems.size(), bw, bw - 3 * FONT_H);
        for (size_t i = 0; i < g_settingsState.pickerItems.size(); i++) {
            const auto &it = g_settingsState.pickerItems[i];
            ESP_LOGI(TAG, "  [%u] label='%s' w=%d path='%s'", (unsigned)i, it.label.c_str(),
                     g_font.textWidth(it.label.c_str()), it.value.c_str());
        }
    }
}

static void closePicker() {
    g_settingsState.pickerOpen = false;
    g_settingsState.pickerField = -1;
    g_settingsState.pickerSel = 0;
    g_settingsState.pickerScroll = 0;
    g_settingsState.pickerItems.clear();
    g_settingsState.pickerItems.shrink_to_fit();
}

static void drawPicker() {
    drawBrowseListBody();   // 底下的列表照画（**不提交**），浮层盖在上面，最后一起提交

    auto &items = g_settingsState.pickerItems;
    const int n = (int)items.size();
    int bx, by, bw, bh, rows;
    pickerBoxRect(items, &bx, &by, &bw, &bh, &rows);
    if (g_settingsState.pickerSel < g_settingsState.pickerScroll)
        g_settingsState.pickerScroll = g_settingsState.pickerSel;
    if (g_settingsState.pickerSel >= g_settingsState.pickerScroll + rows)
        g_settingsState.pickerScroll = g_settingsState.pickerSel - rows + 1;

    u8g2_SetDrawColor(g_u8g2, 1);
    u8g2_DrawBox(g_u8g2, bx, by, bw, bh);
    u8g2_SetDrawColor(g_u8g2, 0);
    u8g2_DrawFrame(g_u8g2, bx, by, bw, bh);

    const SettingField &f = SETTINGS_FIELDS[g_settingsState.pickerField];
    const std::string cur = pickerCurValue(f.key);
    // 标题居中：弹层本身是水平居中的（pickerBoxRect 的 bx=(SCREEN_W-w)/2），所以直接用
    // 整屏居中画，和框内居中等价 —— 选项行本来就是这么居中的（见下面 tx 那两行）。
    ui_draw_text_centered(by + 8 + g_font.ascent(), f.label, false, true);
    u8g2_DrawHLine(g_u8g2, bx + 4, by + 8 + FONT_H + 2, bw - 8);

    for (int i = 0; i < rows && g_settingsState.pickerScroll + i < n; i++) {
        int idx = g_settingsState.pickerScroll + i;
        int y = pickerRowY(by, i);
        bool sel = (idx == g_settingsState.pickerSel);
        // 现用值前面点一个 ●：弹层比循环式好在"有哪些值、现在是哪个"一眼看清。
        const bool isCur = (items[idx].value == cur);
        std::string lb = std::string(isCur ? "\xe2\x97\x8f " : "  ") +
                         fitText(items[idx].label, bw - 3 * FONT_H);
        int tx = bx + (bw - g_font.textWidth(lb.c_str())) / 2;
        if (sel) {
            u8g2_SetDrawColor(g_u8g2, 0);
            u8g2_DrawBox(g_u8g2, bx + 4, y - g_font.ascent() - 2, bw - 8, FONT_H + 4);
            u8g2_SetDrawColor(g_u8g2, 1);
            g_font.drawText(tx, y, lb.c_str(), true);
            u8g2_SetDrawColor(g_u8g2, 0);
        } else {
            g_font.drawText(tx, y, lb.c_str(), false);
        }
    }
    ui_draw_status("↑↓ 选择  Enter 确定  Esc 取消", "");
    ui_commit();
}

// 弹层按键：↑↓/点按移动，回车落定，Esc 取消；点浮层外也算取消。
static AppState settingsPickerHandle(int key, ScreenContext &ctx) {
    if (!g_settingsState.pickerOpen || g_settingsState.pickerItems.empty()) {
        closePicker();
        drawBrowseList();
        return APP_SETTINGS;
    }
    auto &items = g_settingsState.pickerItems;
    const int n = (int)items.size();
    int bx, by, bw, bh, rows;
    pickerBoxRect(items, &bx, &by, &bw, &bh, &rows);

    if (key == KEY_UP || key == 'k') {
        if (g_settingsState.pickerSel > 0) g_settingsState.pickerSel--;
    } else if (key == KEY_DOWN || key == 'j') {
        if (g_settingsState.pickerSel < n - 1) g_settingsState.pickerSel++;
    } else if (key == KEY_PAGE_UP || key == KEY_PAGE_DOWN) {
        // 整页翻是**挪窗口**，选中行按同样的位移跟着走（同 screen_reader 的弹层）：
        // 选中行本来就在页首时，只按"选中行 ±rows"算的话窗口只挪得动一行，
        // 上下滑看着就像没翻页。
        int top = g_settingsState.pickerScroll + (key == KEY_PAGE_DOWN ? rows : -rows);
        const int maxTop = n > rows ? n - rows : 0;
        if (top < 0) top = 0;
        if (top > maxTop) top = maxTop;
        int t = g_settingsState.pickerSel + (top - g_settingsState.pickerScroll);
        if (t < 0) t = 0;
        if (t > n - 1) t = n - 1;
        g_settingsState.pickerSel = t;
        g_settingsState.pickerScroll = top;
    } else if (key == 0x0A || key == 0x0D) {
        int tx, ty;
        if (input_tap_xy(&tx, &ty)) {
            if (tx < bx || tx >= bx + bw || ty < by || ty >= by + bh) {
                closePicker();          // 点浮层外 = 取消
                drawBrowseList();
                return APP_SETTINGS;
            }
            int top = pickerRowY(by, 0) - g_font.ascent() - 2;
            int r = (ty - top) / LINE_SPACING;
            if (ty < top || r < 0 || r >= rows || g_settingsState.pickerScroll + r >= n) {
                drawPicker();           // 点在标题/分隔线上：忽略这一次点按
                return APP_SETTINGS;
            }
            g_settingsState.pickerSel = g_settingsState.pickerScroll + r;
        }
        // 先把 key 抄出来再关：closePicker() 会把 pickerField 复位成 -1，
        // 那之后 SETTINGS_FIELDS[] 的引用就是野的了。
        std::string fkey = SETTINGS_FIELDS[g_settingsState.pickerField].key;
        std::string value = items[g_settingsState.pickerSel].value;
        closePicker();
        pickerApply(fkey.c_str(), value, ctx);
        drawBrowseList();
        return APP_SETTINGS;
    } else if (key == 0x1B || key == 'q' || key == 'Q') {
        closePicker();
        drawBrowseList();
        return APP_SETTINGS;
    }

    drawPicker();
    return APP_SETTINGS;
}

static std::string settingsTrim(const std::string &s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

static void invalidateDictCache() {
    g_settingsState.dictCacheValid = false;
    g_settingsState.dictFilterCacheValid = false;
}

static const std::vector<IME::UserEntryView> &currentDictEntries() {
    if (!g_settingsState.dictCacheValid || g_settingsState.dictCacheKind != g_settingsState.dictKind) {
        g_settingsState.dictEntriesCache = g_ime.userDictEntries(g_settingsState.dictKind);
        g_settingsState.dictCacheKind = g_settingsState.dictKind;
        g_settingsState.dictCacheValid = true;
        g_settingsState.dictFilterCacheValid = false;
    }
    return g_settingsState.dictEntriesCache;
}

static const std::vector<int> &dictFilteredIndices(const std::vector<IME::UserEntryView> &entries);

static void drawDictChoose() {
    ui_clear();
    ui_draw_text_centered(FONT_H, "词库管理", false, true);
    ui_draw_text(8, FONT_H * 3, "固定词库", g_settingsState.dictSelection == 0);
    ui_draw_text(8, FONT_H * 4, "动态词库", g_settingsState.dictSelection == 1);
    ui_draw_text(8, FONT_H * 5, "联想词库", g_settingsState.dictSelection == 2);
    ui_draw_status("Enter进入 Esc返回", "");
    ui_commit();
}

// 词典表的几何：表头基线 / 一屏行数。drawDictList 与按键翻页共用同一份
// ——两处各写一遍式子迟早会漂（ui/list_view.h 开头那段讲的就是这个坑）。
static int dictListLastBase() { return STATUS_Y - FONT_H + g_font.ascent(); }
static int dictListVisibleRows() {
    int v = (dictListLastBase() - FONT_H * 2) / FONT_H;
    return v < 1 ? 1 : v;
}

static void drawDictList(bool doCommit = true) {
    auto &entries = currentDictEntries();
    auto &filtered = dictFilteredIndices(entries);
    int total = (int)filtered.size();
    if (g_settingsState.dictSelection >= total) g_settingsState.dictSelection = total - 1;
    if (g_settingsState.dictSelection < 0) g_settingsState.dictSelection = 0;

    ui_clear();
    char title[64];
    snprintf(title, sizeof(title), "%s %d/%d", dictKindLabel(g_settingsState.dictKind),
             (int)entries.size(), dictKindLimit(g_settingsState.dictKind));
    ui_draw_text_centered(FONT_H, title, false, true);

    // 表格底部贴住状态栏分割线:由最后一行单元格底边=STATUS_Y反推表头基线,
    // 空出的顶部余量让表头整体下移,能多放一行就多放一行
    int lastBase = dictListLastBase();
    int visible = (lastBase - FONT_H * 2) / FONT_H;
    int startY = lastBase - visible * FONT_H;
    if (visible < 1) visible = 1;
    if (g_settingsState.dictSelection < g_settingsState.dictScroll)
        g_settingsState.dictScroll = g_settingsState.dictSelection;
    if (g_settingsState.dictSelection >= g_settingsState.dictScroll + visible)
        g_settingsState.dictScroll = g_settingsState.dictSelection - visible + 1;

    if (total == 0) {
        ui_draw_text_centered(FONT_H * 4, entries.empty() ? "暂无词条" : "无匹配词条");
    } else {
        u8g2_SetDrawColor(g_u8g2, 0);
        ui_draw_text(8, startY, "  编码", false, true);
        ui_draw_text(120, startY, "候选词", false, true);
        ui_draw_text(320, startY, "频次", false, true);
        for (int i = 0; i < visible && g_settingsState.dictScroll + i < total; i++) {
            int viewIdx = g_settingsState.dictScroll + i;
            int idx = filtered[viewIdx];
            bool sel = viewIdx == g_settingsState.dictSelection;
            bool marked = g_settingsState.dictSelected.count(idx) > 0;
            int rowY = startY + (i + 1) * FONT_H;
            if (rowY >= STATUS_Y) break;
            if (sel) {
                u8g2_SetDrawColor(g_u8g2, 0);
                u8g2_DrawBox(g_u8g2, 0, rowY - g_font.ascent(), SCREEN_W, FONT_H);
                u8g2_SetDrawColor(g_u8g2, 1);
            } else {
                u8g2_SetDrawColor(g_u8g2, 0);
            }
            char mark[2] = { marked ? '*' : ' ', 0 };
            char count[16];
            snprintf(count, sizeof(count), "%d", entries[idx].count);
            g_font.drawText(8, rowY, mark, false);
            g_font.drawText(28, rowY, entries[idx].code.c_str(), false);
            g_font.drawText(120, rowY, entries[idx].word.c_str(), false);
            g_font.drawText(320, rowY, count, false);
            u8g2_SetDrawColor(g_u8g2, 0);
        }
    }
    char left[64];
    if (!g_settingsState.dictNotice.empty()) {
        snprintf(left, sizeof(left), "%s", g_settingsState.dictNotice.c_str());
    } else {
        snprintf(left, sizeof(left), "a加 d删 i导入 e导出 已选%d", (int)g_settingsState.dictSelected.size());
    }
    std::string right = g_settingsState.dictSearchBuffer.empty() ? "Space多选" : ("/" + g_settingsState.dictSearchBuffer);
    if (g_settingsState.dictSearching)
        right += " " + imeStatusLabel(g_settingsState.dictSearchImeActive);
    ui_draw_status(left, right.c_str());
    if (doCommit) ui_commit();
}

static void drawDictSearch() {
    drawDictList(false);
    const bool vk = editorVkVisible();
    bool composing = g_settingsState.dictSearchImeActive && g_ime.composing() && !vk;
    const int boxH = FONT_H * 2 + 8;
    // 输入框的位置：键盘弹着时贴到面板顶边，否则照旧贴状态栏（候选条在时再往上让 67px）。
    int y = vk ? (editorVkTop() - boxH - 8)
               : (composing ? (STATUS_Y - 67 - boxH - 2) : (STATUS_Y - boxH - 2));
    if (y < FONT_H * 2) y = FONT_H * 2;
    // ui_draw_status 结束时 draw color=1(白),此处必须显式设色:白底黑字
    u8g2_SetDrawColor(g_u8g2, 1);
    u8g2_DrawBox(g_u8g2, 0, y, SCREEN_W, boxH);
    u8g2_SetDrawColor(g_u8g2, 0);
    ui_draw_text(4, y + FONT_H, "搜索编码或候选词");
    std::string display = g_settingsState.dictSearchBuffer.empty() ? " " : g_settingsState.dictSearchBuffer;
    ui_draw_text(4, y + FONT_H * 2, display.c_str());
    int cx = g_font.textWidth(display.substr(0, g_settingsState.dictSearchCursor).c_str());
    u8g2_DrawBox(g_u8g2, 4 + cx, y + FONT_H * 2 + 4, 8, 3);
    u8g2_SetDrawColor(g_u8g2, 1);
    if (vk) editorVkDraw();
    else if (composing) drawIMEUIWithStatusBar();
    ui_commit();
}

static void drawDictAdd() {
    ui_clear();
    char title[64];
    snprintf(title, sizeof(title), "添加%s", dictKindLabel(g_settingsState.dictKind));
    ui_draw_text_centered(FONT_H, title, false, true);
    ui_draw_text(4, FONT_H * 3, g_settingsState.dictKind == IME::PREDICT_DICT ? "格式: 上字 候选" : "格式: code word");
    std::string display = g_settingsState.dictAddBuffer.empty() ? " " : g_settingsState.dictAddBuffer;
    ui_draw_text(4, FONT_H * 4, display.c_str());
    int cx = g_font.textWidth(display.substr(0, g_settingsState.dictAddCursor).c_str());
    u8g2_SetDrawColor(g_u8g2, 0);
    u8g2_DrawBox(g_u8g2, 4 + cx, FONT_H * 4 + 4, 8, 3);
    u8g2_SetDrawColor(g_u8g2, 1);
    ui_draw_status("Enter确定 Esc取消", imeStatusLabel(g_settingsState.dictAddImeActive).c_str());
    if (editorVkVisible()) editorVkDraw();   // 键盘面板自带候选区，原来的候选条就不画了
    else if (g_settingsState.dictAddImeActive && g_ime.composing())
        drawIMEUIWithStatusBar();
    ui_commit();
}

static bool parseDictAdd(const std::string &line, std::string &code, std::string &word) {
    std::string s = settingsTrim(line);
    size_t sp = s.find_first_of(" \t");
    if (sp == std::string::npos) return false;
    code = settingsTrim(s.substr(0, sp));
    std::string rest = settingsTrim(s.substr(sp + 1));
    size_t sp2 = rest.find_first_of(" \t");
    word = settingsTrim(sp2 == std::string::npos ? rest : rest.substr(0, sp2));
    return !code.empty() && !word.empty();
}

static bool parseDictImportLine(const std::string &line, std::string &code,
                                std::string &word, int &count, bool &trad) {
    if (!parseDictAdd(line, code, word)) return false;
    count = 1;
    trad = false;
    std::string s = settingsTrim(line);
    size_t sp1 = s.find_first_of(" \t");
    if (sp1 == std::string::npos) return true;
    std::string rest = settingsTrim(s.substr(sp1 + 1));
    size_t sp2 = rest.find_first_of(" \t");
    if (sp2 == std::string::npos) return true;
    std::string tail = settingsTrim(rest.substr(sp2 + 1));
    size_t sp3 = tail.find_first_of(" \t");
    std::string countText = sp3 == std::string::npos ? tail : tail.substr(0, sp3);
    bool countOk = !countText.empty();
    int parsed = 0;
    for (char c : countText) {
        if (c < '0' || c > '9') { countOk = false; break; }
        parsed = parsed * 10 + (c - '0');
    }
    if (countOk && parsed > 0) count = parsed;
    if (sp3 != std::string::npos) {
        std::string flag = settingsTrim(tail.substr(sp3 + 1));
        trad = flag == "1" || flag == "t" || flag == "T";
    }
    return true;
}

struct DictImportResult {
    bool opened = false;
    int imported = 0;
    int skipped = 0;
    int malformed = 0;
};

static std::string dictImportSummary(const DictImportResult &r) {
    if (!r.opened) return "未找到导入文件";
    char buf[64];
    snprintf(buf, sizeof(buf), "导入%d 跳过%d 错%d", r.imported, r.skipped, r.malformed);
    return buf;
}

static bool exportCurrentDict() {
    auto &entries = currentDictEntries();
    const char *path = dictExportPath(g_settingsState.dictKind);
    mkdir("/sdcard/settings", 0777);
    FILE *f = fopen(path, "w");
    if (!f) return false;
    for (auto &e : entries) {
        std::string line = e.code + " " + e.word + " " + std::to_string(e.count)
                         + (e.trad ? " 1" : "") + "\n";
        fwrite(line.data(), 1, line.size(), f);
    }
    return fclose(f) == 0;
}

static DictImportResult importCurrentDict() {
    DictImportResult result;
    const char *path = dictImportPath(g_settingsState.dictKind);
    FILE *f = fopen(path, "r");
    if (!f) return result;
    result.opened = true;
    std::vector<IME::UserEntryView> parsedEntries;
    std::vector<std::string> badLines;
    char buf[256];
    while (fgets(buf, sizeof(buf), f)) {
        std::string raw = settingsTrim(buf);
        if (raw.empty() || raw[0] == '#') continue;
        std::string code, word;
        int count = 1;
        bool trad = false;
        if (!parseDictImportLine(buf, code, word, count, trad)) {
            result.malformed++;
            badLines.push_back(raw);
            continue;
        }
        parsedEntries.push_back({code, word, count, trad});
    }
    fclose(f);
    const char *errPath = dictImportErrorPath(g_settingsState.dictKind);
    if (badLines.empty()) {
        remove(errPath);
    } else {
        FILE *ef = fopen(errPath, "w");
        if (ef) {
            for (auto &line : badLines) {
                fwrite(line.data(), 1, line.size(), ef);
                fwrite("\n", 1, 1, ef);
            }
            fclose(ef);
        }
    }
    int skipped = 0;
    result.imported = g_ime.addUserDictEntries(g_settingsState.dictKind, parsedEntries, &skipped);
    result.skipped = skipped;
    return result;
}

static const std::vector<int> &dictFilteredIndices(const std::vector<IME::UserEntryView> &entries) {
    std::string q = settingsTrim(g_settingsState.dictSearchBuffer);
    if (g_settingsState.dictFilterCacheValid &&
        g_settingsState.dictFilterCacheQuery == q &&
        g_settingsState.dictFilteredCache.size() <= entries.size())
        return g_settingsState.dictFilteredCache;
    g_settingsState.dictFilteredCache.clear();
    g_settingsState.dictFilteredCache.reserve(entries.size());
    for (int i = 0; i < (int)entries.size(); i++) {
        if (q.empty() || entries[i].code.find(q) != std::string::npos ||
            entries[i].word.find(q) != std::string::npos)
            g_settingsState.dictFilteredCache.push_back(i);
    }
    g_settingsState.dictFilterCacheQuery = q;
    g_settingsState.dictFilterCacheValid = true;
    return g_settingsState.dictFilteredCache;
}

enum class SettingsAsyncOp { None, FlomoToken, SyncTime, Backup, Restore };
static SettingsAsyncOp s_settingsAsyncOp = SettingsAsyncOp::None;
static AppAsyncJob s_settingsAsync;
static bool s_settingsAsyncRestart = false;

static void settingsFlomoTokenTask(void *) {
    std::string email = g_settings.flomoEmail();
    std::string pass = g_settings.flomoPassword();
    if (email.empty() || pass.empty()) {
        s_settingsAsync.finish("请先设置Flomo邮箱和密码");
        vTaskDelete(nullptr);
        return;
    }
    bool wifiWas = false;
    bool ok = app_connect_wifi_from_settings(&wifiWas);
    if (!ok) {
        s_settingsAsync.finish("WiFi连接失败");
    } else if (s_settingsAsync.cancelled()) {
        s_settingsAsync.finish("已取消");
    } else {
        g_flomo.configure(email, pass);
        std::string token = g_flomo.login();
        if (!token.empty()) {
            g_flomo.setCachedToken(token);
            s_settingsAsync.finish("Token生成成功");
        } else {
            std::string why = g_flomo.lastError();
            s_settingsAsync.finish(why.empty() ? "Flomo登录失败" : ("登录失败: " + why));
        }
    }
    app_disconnect_wifi_if_needed(wifiWas);
    vTaskDelete(nullptr);
}

static void settingsSyncTimeTask(void *) {
    bool wifiWas = false;
    bool ok = app_connect_wifi_from_settings(&wifiWas);
    if (!ok) {
        s_settingsAsync.finish("WiFi连接失败");
        vTaskDelete(nullptr);
        return;
    }
    if (!wifiWas) vTaskDelay(pdMS_TO_TICKS(500));
    if (s_settingsAsync.cancelled()) {
        s_settingsAsync.finish("已取消");
        app_disconnect_wifi_if_needed(wifiWas);
        vTaskDelete(nullptr);
        return;
    }
    std::string ntp = g_settings.ntpServer();
    std::string tz = g_settings.timezone();
    if (tz.empty()) tz = "CST-8";
    if (ntp.empty()) {
        s_settingsAsync.finish("请先设置NTP服务器");
    } else {
        esp_sntp_stop();
        esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
        esp_sntp_setservername(0, ntp.c_str());
        esp_sntp_set_sync_status(SNTP_SYNC_STATUS_RESET);
        esp_sntp_init();
        setenv("TZ", tz.c_str(), 1);
        tzset();
        time_t now = 0;
        for (int i = 0; i < 100; i++) {
            if (s_settingsAsync.cancelled()) break;
            vTaskDelay(pdMS_TO_TICKS(200));
            if (esp_sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
                time(&now);
                break;
            }
        }
        esp_sntp_stop();
        if (s_settingsAsync.cancelled()) {
            s_settingsAsync.finish("已取消");
        } else if (now > 1704067200) {
            struct tm tmv;
            localtime_r(&now, &tmv);
            char ts[64];
            strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tmv);
            g_rtc.setTime(now);
            s_settingsAsync.finish(std::string("同步成功: ") + ts);
        } else {
            s_settingsAsync.finish("时间同步失败");
        }
    }
    app_disconnect_wifi_if_needed(wifiWas);
    vTaskDelete(nullptr);
}

static void settingsBackupTask(void *) {
    g_settings.flush();
    const esp_err_t err = settings_backup_save();
    s_settingsAsync.finish(
        err == ESP_OK ? "已备份到 TF 卡 settings_backup/"
        : err == ESP_ERR_INVALID_STATE ? "未识别到 TF 卡，请插卡后重试"
                                       : "备份失败，请检查卡剩余空间");
    vTaskDelete(nullptr);
}

static void settingsRestoreTask(void *) {
    g_settings.flush();
    const esp_err_t err = settings_backup_restore();
    if (err == ESP_OK) {
        s_settingsAsyncRestart = true;
        s_settingsAsync.finish("已恢复，正在重启...");
    } else {
        s_settingsAsync.finish(err == ESP_ERR_INVALID_STATE ? "未识别到 TF 卡，请插卡后重试"
                                                            : "备份不可用，恢复未执行");
    }
    vTaskDelete(nullptr);
}

static bool startSettingsAsync(SettingsAsyncOp op) {
    if (s_settingsAsync.state() == AppAsyncState::Running) return false;
    s_settingsAsyncOp = op;
    std::string title, busy;
    if (op == SettingsAsyncOp::FlomoToken) {
        title = "Flomo Token";
        busy = "正在生成...";
    } else if (op == SettingsAsyncOp::SyncTime) {
        title = "网络同步时间";
        busy = "正在同步...";
    } else if (op == SettingsAsyncOp::Backup) {
        title = "备份设置与记录";
        busy = "正在备份...";
    } else {
        title = "从备份恢复";
        busy = "正在恢复...";
    }
    s_settingsAsyncRestart = false;
    s_settingsAsync.begin(title, busy);
    TaskFunction_t fn = settingsSyncTimeTask;
    if (op == SettingsAsyncOp::FlomoToken) fn = settingsFlomoTokenTask;
    else if (op == SettingsAsyncOp::Backup) fn = settingsBackupTask;
    else if (op == SettingsAsyncOp::Restore) fn = settingsRestoreTask;
    if (!s_settingsAsync.start(fn, "settings_async", 8192)) {
        s_settingsAsync.failToStart("任务启动失败");
        return false;
    }
    return true;
}

static bool settingsAsyncHandle(int key) {
    AppAsyncState state = s_settingsAsync.state();
    if (state == AppAsyncState::Idle) return false;
    if (state == AppAsyncState::Running) {
        if (key == 'q' || key == 'Q' || key == 0x1B) {
            s_settingsAsync.cancel();
        }
        if (!s_settingsAsync.drawn) {
            // 走公共件：这里原来自己画了一遍，顺序是"先标题后 ui_show_message_centered"，
            // 而后者内部会 ui_clear() —— 标题被清掉，busy 画面只剩一行 "正在xx..."。
            // ui_feedback_titled_message 用的就是正确顺序（先框后标题），别再抄第二份。
            ui_feedback_titled_message(s_settingsAsync.title().c_str(),
                                       s_settingsAsync.busy().c_str(), 0);
            s_settingsAsync.drawn = true;
        }
        vTaskDelay(pdMS_TO_TICKS(80));
        return true;
    }
    if (s_settingsAsync.untilUs == 0 || key > 0) {
        ui_feedback_message(s_settingsAsync.result().c_str(), 0);
        s_settingsAsync.untilUs = esp_timer_get_time() + 1800LL * 1000;
        return true;
    }
    if (esp_timer_get_time() < s_settingsAsync.untilUs) return true;
    if (s_settingsAsyncRestart) {
        vTaskDelay(pdMS_TO_TICKS(200));
        esp_restart();
    }
    s_settingsAsync.reset();
    s_settingsAsyncOp = SettingsAsyncOp::None;
    drawBrowseList();
    return true;
}

// ── 设置分类列表(顶层) ───────────────────────────────────────────────────
// 版式与字段列表完全一致：第一行标题，选项从第二行基线起，触屏按行映射。
// 分类项显示本分类当前可见的项数，进分类后隐藏项(打字机/竖排专属)才真正消失。
static int settingsListTop() { return 2 * FONT_H - g_font.ascent(); }
static int settingsListVisible() { return (SCREEN_H - 2 * FONT_H + FONT_H - 1) / FONT_H; }

static ListView settingsCategoryListView() {
    ListView lv;
    lv.top = settingsListTop();
    lv.itemH = FONT_H;
    lv.count = CAT_COUNT;
    lv.rows = settingsListVisible();
    lv.sel = g_settingsState.selection;
    lv.first = 0;
    listViewScroll(lv);
    return lv;
}

static ListView settingsBrowseListView() {
    ListView lv;
    lv.top = settingsListTop();
    lv.itemH = FONT_H;
    lv.count = fieldVisibleCount();
    lv.rows = settingsListVisible();
    lv.sel = g_settingsState.selection;
    lv.first = g_settingsState.scroll;
    listViewFollow(lv);
    return lv;
}

static void drawSettingsCategories() {
    ui_clear();
    ui_draw_text_centered(FONT_H, "设置", false, true);
    ListView lv = settingsCategoryListView();
    g_settingsState.selection = lv.sel;
    for (int row = 0; row < lv.rows && row < lv.count; row++) {
        int i = lv.first + row;
        bool sel = (i == lv.sel);
        char buf[64];
        snprintf(buf, sizeof(buf), "▶ %s (%d)", SETTINGS_CAT_NAMES[i], catFieldCount(i));
        ui_draw_text(8, 2 * FONT_H + row * FONT_H, buf, sel);
    }
    ui_commit();
}

// ── Screen entry points ──────────────────────────────────────────────────
void screen_settings_init() {
    s_cat = CAT_GEN;
    s_cfgRestoreConfirm = false;   // 进这一屏就作废上一次没确认完的恢复
    g_settingsState.selection = g_settingsState.scroll = 0;
    g_settingsState.editing = false;
    g_settingsState.editBuffer.clear();
    g_settingsState.editCursor = 0;
    g_settingsState.imeActive = false;
    g_settingsState.mode = SETTINGS_CATEGORY;
    g_settingsState.dictKind = IME::FIXED_DICT;
    g_settingsState.dictSelection = 0;
    g_settingsState.dictScroll = 0;
    g_settingsState.dictSelected.clear();
    g_settingsState.dictAddBuffer.clear();
    g_settingsState.dictAddCursor = 0;
    g_settingsState.dictAddImeActive = false;
    g_settingsState.dictSearching = false;
    g_settingsState.dictSearchBuffer.clear();
    g_settingsState.dictSearchCursor = 0;
    g_settingsState.dictSearchImeActive = false;
    g_settingsState.dictNotice.clear();
    invalidateDictCache();
    g_settingsState.pickerOpen = false;
    g_settingsState.pickerField = -1;
    g_settingsState.pickerSel = 0;
    g_settingsState.pickerScroll = 0;
    g_settingsState.pickerItems.clear();
    g_settingsState.pickerItems.shrink_to_fit();
    textSelReset();   // 上次留下的选区/按钮条作废（会话是模块级静态量，会跨屏残留）
}

// 当前"可打字的子状态"里的输入法开关（三处各有一个标志）。软键盘点按泵要按它
// 决定点中/英时是"翻开"还是"切中英"。
static bool settingsImeOn() {
    if (g_settingsState.editing) return g_settingsState.imeActive;
    if (g_settingsState.mode == SETTINGS_DICT_ADD) return g_settingsState.dictAddImeActive;
    if (g_settingsState.mode == SETTINGS_DICT_LIST && g_settingsState.dictSearching)
        return g_settingsState.dictSearchImeActive;
    return false;
}

static void settingsImeTurnOn() {
    if (g_settingsState.editing) g_settingsState.imeActive = true;
    else if (g_settingsState.mode == SETTINGS_DICT_ADD) g_settingsState.dictAddImeActive = true;
    else if (g_settingsState.mode == SETTINGS_DICT_LIST) g_settingsState.dictSearchImeActive = true;
    g_ime.setActive(true);
}

// ── 文本字段编辑：折行 + 屏幕几何 ────────────────────────────────────────
// 字段编辑区是**折行多行**的：按宽度把缓冲切成若干显示行，一行行画。绘制与触摸
// 选区（共享件 text_sel）从这里取同一套几何，免得两边算岔。
// lines 是 display 上的字节区间；masked 时 display 与 editBuffer **等长**
// （每个字节一个 '*'），所以区间可以直接当 editBuffer 的偏移用。
static void settingsEditLayout(const std::string &display,
                               std::vector<std::pair<int,int>> &lines, int &textY) {
    const int sepY = FONT_H + g_font.descent() + 4;
    textY = sepY + 4 + g_font.ascent();
    lines.clear();
    const int maxW = SCREEN_W - 8;
    int lineStart = 0;
    while (lineStart < (int)display.length()) {
        int lineEnd = lineStart;
        int lastGood = lineStart;
        while (lineEnd <= (int)display.length()) {
            int w = g_font.textWidth(display.substr(lineStart, lineEnd - lineStart).c_str());
            if (w > maxW) {
                lineEnd = lastGood;
                break;
            }
            lastGood = lineEnd;
            lineEnd++;
            while (lineEnd < (int)display.length() &&
                   ((unsigned char)display[lineEnd] & 0xC0) == 0x80)
                lineEnd++;
        }
        if (lineEnd <= lineStart) lineEnd = lastGood;
        if (lineEnd <= lineStart) lineEnd = display.length();
        lines.push_back({lineStart, lineEnd});
        lineStart = lineEnd;
    }
    if (lines.empty()) lines.push_back({0, 0});   // 空字段也占一行，长按才点得中
}

// 显示行 → 触摸选区的行表。字段没有换行，所以"整行"就是"这一折行"。
static void settingsEditLineTable(const std::vector<std::pair<int,int>> &lines, int textY,
                                  std::vector<TextSelLine> &out, int &bottom) {
    out.clear();
    for (size_t i = 0; i < lines.size(); i++) {
        TextSelLine ln;
        ln.start = lines[i].first;
        ln.end = lines[i].second;
        ln.x0 = 4;
        ln.baseline = textY + (int)i * FONT_H;
        out.push_back(ln);
    }
    bottom = editorVkVisible() ? editorVkTop() : SCREEN_H;
}

// 字段编辑整屏重绘（含触摸选区的反白 + 按钮条）。
static void drawSettingsEdit() {
    ui_clear();
    auto &f = SETTINGS_FIELDS[fieldAt(g_settingsState.selection)];
    ui_draw_text_centered(FONT_H, f.label, false, true);
    const int sepY = FONT_H + g_font.descent() + 4;
    u8g2_DrawHLine(g_u8g2, 0, sepY, SCREEN_W);

    std::string display = g_settingsState.editBuffer;
    if (f.masked) display = std::string(display.length(), '*');

    std::vector<std::pair<int,int>> lines;
    int textY = 0;
    settingsEditLayout(display, lines, textY);

    for (int i = 0; i < (int)lines.size(); i++) {
        std::string lineText = display.substr(lines[i].first, lines[i].second - lines[i].first);
        if (lineText.empty() && i == 0) lineText = " ";
        ui_draw_text(4, textY + i * FONT_H, lineText.c_str());
    }

    int cursorLine = 0;
    int cursorX = 0;
    for (int i = 0; i < (int)lines.size(); i++) {
        if (g_settingsState.editCursor >= lines[i].first && g_settingsState.editCursor <= lines[i].second) {
            cursorLine = i;
            std::string beforeCursor = display.substr(lines[i].first, g_settingsState.editCursor - lines[i].first);
            cursorX = g_font.textWidth(beforeCursor.c_str());
            break;
        }
    }

    std::string cursorChar = display.substr(g_settingsState.editCursor, 1);
    int cw = cursorChar.empty() || cursorChar == " " ? 8 : g_font.textWidth(cursorChar.c_str());
    u8g2_SetDrawColor(g_u8g2, 0);
    u8g2_DrawBox(g_u8g2, 4 + cursorX, textY + cursorLine * FONT_H + 4, cw, 3);
    u8g2_SetDrawColor(g_u8g2, 1);

    if (editorVkVisible()) editorVkDraw();
    else if (g_settingsState.imeActive && g_ime.composing()) drawIMEUIFullscreen();
    else ui_draw_status("Enter保存 Esc取消", imeStatusLabel(g_settingsState.imeActive).c_str());

    // 触摸选区的反白 + 按钮条（会话没开就是空操作）。画在最上面：键盘在它下面。
    // 口令类字段（masked）不给触摸选区：显示的是 '*'，反白位置和真实字节对不上，
    // 长按维持原来的"返回"。
    if (!f.masked) {
        std::vector<TextSelLine> tl;
        int bottom = 0;
        settingsEditLineTable(lines, textY, tl, bottom);
        TextSelView view{tl.data(), (int)tl.size(), bottom};
        textSelDraw(display, view);
    }
    ui_commit();
}

AppState screen_settings_handle(int key, ScreenContext &ctx) {
    if (settingsAsyncHandle(key)) return APP_SETTINGS;

    // ── 轮换制设置项的选项弹层（浮层，优先于其它一切按键）──
    if (g_settingsState.pickerOpen) return settingsPickerHandle(key, ctx);

    // ── 虚拟键盘：落在键盘面板上的点按翻译成键码，喂给下面的输入分支 ──
    // 面板打开时（字段编辑/词典新增/词典搜索，且没连蓝牙键盘）才谈得上；不在面板上
    // 的点按一个字都不动，照旧留给下面 browse 的行点选。
    {
        int vkKey = 0;
        bool turnOn = false;
        if (editorVkPumpTap(settingsImeOn(), &vkKey, &turnOn)) {
            if (turnOn) settingsImeTurnOn();
            key = vkKey;   // 0 = 只是重绘（按下反馈/翻页/切布局）
        }
    }
    // ── 设置分类列表(顶层) ────────────────────────────────────────────
    if (g_settingsState.mode == SETTINGS_CATEGORY) {
        if (key == 'q' || key == 'Q' || key == 0x1B) {
            ctx.nextState = g_quickEdit ? APP_EDITOR : APP_MAIN;
            return ctx.nextState;
        }
        if (key == 'k') key = KEY_UP;
        else if (key == 'j') key = KEY_DOWN;
        {
            ListView lv = settingsCategoryListView();
            if (listViewKey(lv, key)) g_settingsState.selection = lv.sel;
        }
        if (key == 0x0A || key == 0x0D) {
            int tx, ty;
            if (input_tap_xy(&tx, &ty)) {
                ListView lv = settingsCategoryListView();
                int row = listViewHitAt(lv, ty);
                if (row < 0) return APP_SETTINGS;  // 点标题/列表外空白
                g_settingsState.selection = row;
            }
            s_cat = g_settingsState.selection;      // 进入该分类
            g_settingsState.selection = 0;
            g_settingsState.scroll = 0;
            g_settingsState.mode = SETTINGS_BROWSE;
        }
        drawSettingsCategories();
        return APP_SETTINGS;
    }

    // ── User dictionary manager ───────────────────────────────────────
    if (g_settingsState.mode == SETTINGS_DICT_CHOOSE) {
        if (key == 0x1B || key == 'q' || key == 'Q') {
            g_settingsState.mode = SETTINGS_BROWSE;
        } else if (key == KEY_UP || key == 'k') {
            g_settingsState.dictSelection = (g_settingsState.dictSelection + 2) % 3;
        } else if (key == KEY_DOWN || key == 'j') {
            g_settingsState.dictSelection = (g_settingsState.dictSelection + 1) % 3;
        } else if (key == 0x0A || key == 0x0D) {
            g_settingsState.dictKind = dictKindFromSelection(g_settingsState.dictSelection);
            invalidateDictCache();
            g_settingsState.dictSelection = 0;
            g_settingsState.dictScroll = 0;
            g_settingsState.dictSelected.clear();
            g_settingsState.dictSearchBuffer.clear();
            g_settingsState.dictSearchCursor = 0;
            g_settingsState.mode = SETTINGS_DICT_LIST;
        }
        drawDictChoose();
        return APP_SETTINGS;
    }

    if (g_settingsState.mode == SETTINGS_DICT_LIST && g_settingsState.dictSearching) {
        if (g_settingsState.dictSearchImeActive && key != 0) {
            std::string imeOut;
            // 同上：ASCII 兜底那条路还会把 dictSelection/dictScroll 复位（回到第一命中），
            // 这支没有 —— 合并会丢掉那个复位，所以两条路各留各的。
            if (g_ime.handleKey(key, imeOut)) {
                if (!imeOut.empty()) {
                    imeFieldInsert(settingsDictSearchField(), imeOut);
                    g_settingsState.dictFilterCacheValid = false;
                }
                drawDictSearch();
                return APP_SETTINGS;
            }
        }
        if (key == KEY_IME_TOGGLE) {
            g_settingsState.dictSearchImeActive = !g_settingsState.dictSearchImeActive;
            g_ime.setActive(g_settingsState.dictSearchImeActive);
        } else if (key == 0x1B) {
            g_settingsState.dictSearching = false;
            g_settingsState.dictSearchImeActive = false;
            g_ime.setActive(false);
            editorVkAutoHide();
        } else if (key == 0x0A || key == 0x0D) {
            g_settingsState.dictSearching = false;
            g_settingsState.dictSelection = 0;
            g_settingsState.dictScroll = 0;
            g_settingsState.dictSearchImeActive = false;
            g_ime.setActive(false);
            editorVkAutoHide();
        } else if (key == 0x7F || key == 0x08) {
            imeFieldBackspace(settingsDictSearchField());
            g_settingsState.dictSelection = 0;
            g_settingsState.dictScroll = 0;
            g_settingsState.dictFilterCacheValid = false;
        } else if (key == KEY_LEFT) {
            imeFieldMoveLeft(settingsDictSearchField());
        } else if (key == KEY_RIGHT) {
            imeFieldMoveRight(settingsDictSearchField());
        } else if (key >= 0x20 && key <= 0x7E) {
            imeFieldInsert(settingsDictSearchField(), std::string(1, (char)key));
            g_settingsState.dictSelection = 0;
            g_settingsState.dictScroll = 0;
            g_settingsState.dictFilterCacheValid = false;
        }
        drawDictSearch();
        return APP_SETTINGS;
    }

    if (g_settingsState.mode == SETTINGS_DICT_LIST) {
        auto &entries = currentDictEntries();
        auto &filtered = dictFilteredIndices(entries);
        int total = (int)filtered.size();
        if (key == 0x1B || key == 'q' || key == 'Q') {
            g_settingsState.mode = SETTINGS_DICT_CHOOSE;
            g_settingsState.dictSelected.clear();
            g_settingsState.dictSearchBuffer.clear();
            g_settingsState.dictNotice.clear();
        } else if (key == KEY_UP || key == 'k') {
            if (g_settingsState.dictSelection > 0) g_settingsState.dictSelection--;
        } else if (key == KEY_DOWN || key == 'j') {
            if (g_settingsState.dictSelection < total - 1) g_settingsState.dictSelection++;
        } else if (key == KEY_PAGE_UP || key == KEY_PAGE_DOWN) {
            // 触摸上下滑 = 整页翻（与分类/字段列表同一条规矩）：一步一屏，高亮跟着页走，
            // 一屏放得下就没得翻（原先这支没有 KEY_PAGE 分支，上下滑在这里什么都不做）。
            const int pageRows = dictListVisibleRows();
            if (total > pageRows) {
                int t = g_settingsState.dictSelection + (key == KEY_PAGE_DOWN ? pageRows : -pageRows);
                if (t < 0) t = 0;
                if (t > total - 1) t = total - 1;
                g_settingsState.dictSelection = t;
            }
        } else if (key == '/') {
            g_settingsState.dictSearching = true;
            g_settingsState.dictSearchCursor = (int)g_settingsState.dictSearchBuffer.length();
            g_settingsState.dictSearchImeActive = false;
            g_settingsState.dictNotice.clear();
            editorVkAutoShow();   // 没连蓝牙键盘就把虚拟键盘弹出来（连了则内部不动）
        } else if (key == 'a' || key == 'A') {
            g_settingsState.mode = SETTINGS_DICT_ADD;
            g_settingsState.dictAddBuffer.clear();
            g_settingsState.dictAddCursor = 0;
            g_settingsState.dictAddImeActive = false;
            g_ime.setActive(false);
            g_settingsState.dictNotice.clear();
            editorVkAutoShow();
        } else if (key == ' ' && total > 0) {
            int realIdx = filtered[g_settingsState.dictSelection];
            if (g_settingsState.dictSelected.count(realIdx)) g_settingsState.dictSelected.erase(realIdx);
            else g_settingsState.dictSelected.insert(realIdx);
            g_settingsState.dictNotice.clear();
        } else if ((key == 'd' || key == 'D') && total > 0) {
            std::vector<int> indices;
            if (g_settingsState.dictSelected.empty()) {
                indices.push_back(filtered[g_settingsState.dictSelection]);
            } else {
                for (int idx : g_settingsState.dictSelected) indices.push_back(idx);
            }
            g_ime.removeUserDictEntries(g_settingsState.dictKind, indices);
            invalidateDictCache();
            g_settingsState.dictSelected.clear();
            if (g_settingsState.dictSelection >= total - (int)indices.size())
                g_settingsState.dictSelection = std::max(0, total - (int)indices.size() - 1);
            g_settingsState.dictNotice = "已删除";
        } else if ((key == 'c' || key == 'C') && !entries.empty()) {
            g_ime.clearUserDict(g_settingsState.dictKind);
            invalidateDictCache();
            g_settingsState.dictSelected.clear();
            g_settingsState.dictSelection = 0;
            g_settingsState.dictScroll = 0;
            g_settingsState.dictNotice = "已清空";
        } else if (key == 'e' || key == 'E') {
            g_settingsState.dictNotice = exportCurrentDict() ? "已导出" : "导出失败";
        } else if (key == 'i' || key == 'I') {
            g_settingsState.dictNotice = dictImportSummary(importCurrentDict());
            invalidateDictCache();
        }
        drawDictList();
        return APP_SETTINGS;
    }

    if (g_settingsState.mode == SETTINGS_DICT_ADD) {
        if (g_settingsState.dictAddImeActive && key != 0) {
            std::string imeOut;
            if (imeFieldKeyText(g_ime, key, false, imeOut)) {
                if (!imeOut.empty()) {
                    imeFieldInsert(settingsDictAddField(), imeOut);
                }
                drawDictAdd();
                return APP_SETTINGS;
            }
        }
        if (key == KEY_IME_TOGGLE) {
            g_settingsState.dictAddImeActive = !g_settingsState.dictAddImeActive;
            g_ime.setActive(g_settingsState.dictAddImeActive);
        } else if (key == 0x1B) {
            g_settingsState.mode = SETTINGS_DICT_LIST;
            g_settingsState.dictAddBuffer.clear();
            g_settingsState.dictAddCursor = 0;
            g_settingsState.dictAddImeActive = false;
            g_ime.setActive(false);
            editorVkAutoHide();
        } else if (key == 0x0A || key == 0x0D) {
            std::string code, word;
            if (parseDictAdd(g_settingsState.dictAddBuffer, code, word)) {
                g_ime.addUserDictEntry(g_settingsState.dictKind, code, word);
                invalidateDictCache();
            }
            g_settingsState.mode = SETTINGS_DICT_LIST;
            g_settingsState.dictAddBuffer.clear();
            g_settingsState.dictAddCursor = 0;
            g_settingsState.dictAddImeActive = false;
            g_ime.setActive(false);
            editorVkAutoHide();
        } else if (key == 0x7F || key == 0x08) {
            imeFieldBackspace(settingsDictAddField());
        } else if (key == KEY_LEFT) {
            imeFieldMoveLeft(settingsDictAddField());
        } else if (key == KEY_RIGHT) {
            imeFieldMoveRight(settingsDictAddField());
        } else if (key >= 0x20 && key <= 0x7E) {
            imeFieldInsert(settingsDictAddField(), std::string(1, (char)key));
        }
        drawDictAdd();
        return APP_SETTINGS;
    }

    // ── Edit mode ──────────────────────────────────────────────────────
    if (g_settingsState.editing) {
        // 触摸编辑（共享件 text_sel）：长按字段 → 复制/剪切/粘贴/全选。
        // **排在虚拟键盘泵之后**（键盘点按优先）、**IME 分支之前**：会话靠这里吃掉
        // 拖动/长按键，免得漏进 g_ime（有未上屏组合时未知键会上屏候选字）；也只有
        // 排在这儿，"全选后直接打字"才能先把选区删掉、再把字符放给 IME 插进去。
        {
            auto &f = SETTINGS_FIELDS[fieldAt(g_settingsState.selection)];
            if (!f.masked && (key == KEY_TOUCH_LONG || textSelActive())) {
                int tx = 0, ty = 0;
                const bool hasTap = input_tap_xy(&tx, &ty);
                std::string display = g_settingsState.editBuffer;
                std::vector<std::pair<int,int>> lines;
                int textY = 0;
                settingsEditLayout(display, lines, textY);
                std::vector<TextSelLine> tl;
                int bottom = 0;
                settingsEditLineTable(lines, textY, tl, bottom);
                TextSelView view{tl.data(), (int)tl.size(), bottom};
                if (key == KEY_TOUCH_LONG) {
                    if (hasTap && textSelBegin(g_settingsState.editBuffer, g_settingsState.editCursor,
                                               view, tx, ty)) {
                        g_ime.cancelComposition();
                        drawSettingsEdit();
                        return APP_SETTINGS;
                    }
                    key = 0x1B;   // 没落在字段上 → 维持"长按 = 返回"的老语义
                } else if (key != 0 &&
                           textSelHandleKey(g_settingsState.editBuffer, g_settingsState.editCursor,
                                            view, key, tx, ty, hasTap, &ctx.statusMessage)) {
                    drawSettingsEdit();
                    return APP_SETTINGS;
                }
            } else if (textSelActive()) {
                textSelReset();   // 字段换了 / 进入口令字段 → 会话作废
            }
        }

        if (g_settingsState.imeActive && key != 0) {
            std::string imeOut;
            // 这一支**故意不并进 imeFieldKeyText**：它的重绘是就地单行的（不折行、不叠触摸
            // 选区），而"输入法放行的可打印 ASCII"走链尾是 drawSettingsEdit() —— 两条路画法
            // 不同，合并等于让"输入法开着敲数字"换一套重绘。共享层只用来落串/移光标。
            if (g_ime.handleKey(key, imeOut)) {
                if (!imeOut.empty()) {
                    imeFieldInsert(settingsEditField(), imeOut);
                }
                ui_clear();
                auto &f = SETTINGS_FIELDS[fieldAt(g_settingsState.selection)];
                ui_draw_text_centered(FONT_H, f.label, false, true);
                int sepY = FONT_H + g_font.descent() + 4;
                u8g2_DrawHLine(g_u8g2, 0, sepY, SCREEN_W);

                std::string display = g_settingsState.editBuffer;
                if (f.masked) display = std::string(display.length(), '*');
                if (display.empty()) display = " ";
                int textY = sepY + 4 + g_font.ascent();
                int cx = g_font.textWidth(display.substr(0, g_settingsState.editCursor).c_str());
                ui_draw_text(4, textY, display.c_str());

                std::string cursorChar = display.substr(g_settingsState.editCursor, 1);
                int cw = cursorChar.empty() || cursorChar == " " ? 8 : g_font.textWidth(cursorChar.c_str());
                u8g2_SetDrawColor(g_u8g2, 0);
                u8g2_DrawBox(g_u8g2, 4 + cx, textY + 4, cw, 3);
                u8g2_SetDrawColor(g_u8g2, 1);

                if (editorVkVisible()) editorVkDraw();
                else if (g_ime.composing()) drawIMEUIFullscreen();
                else ui_draw_status("Enter保存 Esc取消", imeStatusLabel(g_settingsState.imeActive).c_str());

                ui_commit();
                return APP_SETTINGS;
            }
        }

        if (key == KEY_IME_TOGGLE) {
            g_settingsState.imeActive = !g_settingsState.imeActive;
            g_ime.setActive(g_settingsState.imeActive);
            key = 0;
        }

        if (key == 0x1B) {
            g_settingsState.editing = false;
            g_settingsState.editBuffer.clear();
            g_settingsState.editCursor = 0;
            g_settingsState.imeActive = false;
            g_ime.setActive(false);
            editorVkAutoHide();
        } else if (key == 0x0A || key == 0x0D) {
            auto &f = SETTINGS_FIELDS[fieldAt(g_settingsState.selection)];
            g_settings.setString(f.key, g_settingsState.editBuffer);
            g_settingsState.editing = false;
            g_settingsState.editBuffer.clear();
            g_settingsState.editCursor = 0;
            g_settingsState.imeActive = false;
            g_ime.setActive(false);
            editorVkAutoHide();
        } else if (key == 0x7F || key == 0x08) {
            imeFieldBackspace(settingsEditField());
        } else if (key == KEY_LEFT) {
            imeFieldMoveLeft(settingsEditField());
        } else if (key == KEY_RIGHT) {
            imeFieldMoveRight(settingsEditField());
        } else if (key >= 0x20 && key <= 0x7E) {
            imeFieldInsert(settingsEditField(), std::string(1, (char)key));
        }

        drawSettingsEdit();
        return APP_SETTINGS;
    }

    // ── Browse mode ────────────────────────────────────────────────────
    // Esc 退回分类列表(不直接回主菜单)：设置是两级菜单，先退一级。
    if (key == 'q' || key == 'Q' || key == 0x1B) {
        g_settingsState.mode = SETTINGS_CATEGORY;
        g_settingsState.selection = s_cat;
        g_settingsState.scroll = 0;
        drawSettingsCategories();
        return APP_SETTINGS;
    }
    if (key == 'k') key = KEY_UP;
    else if (key == 'j') key = KEY_DOWN;
    {
        ListView lv = settingsBrowseListView();
        if (listViewKey(lv, key)) {
            g_settingsState.selection = lv.sel;
            g_settingsState.scroll = lv.first;
        }
    }
    if (key == 'd' || key == 'D') {
        auto &f = SETTINGS_FIELDS[fieldAt(g_settingsState.selection)];
        if (!f.action) g_settings.erase(SETTINGS_FIELDS[fieldAt(g_settingsState.selection)].key);
    }
    if (key == 0x0A || key == 0x0D) {
        // 触摸点选：把点按位置映射到字段行，先选中再激活；点标题/空白则忽略本次点按。
        int tx, ty;
        if (input_tap_xy(&tx, &ty)) {
            ListView lv = settingsBrowseListView();
            int fieldRow = listViewHitAt(lv, ty);
            if (fieldRow < 0) return APP_SETTINGS;
            g_settingsState.selection = fieldRow;
        }
        auto &f = SETTINGS_FIELDS[fieldAt(g_settingsState.selection)];
        if (f.action) {
            // 轮换制的项改成弹出选择式：回车（或点按）弹选项列表，不再一个值一个值地循环。
            if (pickerFieldSupported(f.key)) {
                openPicker(fieldAt(g_settingsState.selection));
                drawPicker();
                return APP_SETTINGS;
            }
            if (strcmp(f.key, "_clock_preview") == 0) {
                StandbyFace face = standbyFaceFromKey(g_settings.getString("clock_face").c_str());
                if (face == StandbyFace::Off) {
                    ui_feedback_message("请先选择待机表盘", 1500);
                } else {
                    standbyClockPreview(face, 3000);
                }
                return APP_SETTINGS;
            }
            if (strcmp(f.key, "_flomo_token") == 0) {
                startSettingsAsync(SettingsAsyncOp::FlomoToken);
                return APP_SETTINGS;
            }
            if (strcmp(f.key, "_sync_time") == 0) {
                startSettingsAsync(SettingsAsyncOp::SyncTime);
                return APP_SETTINGS;
            }
            if (strcmp(f.key, "_file_mgr") == 0) {
                ctx.nextState = APP_FILE_MANAGER;
                return APP_FILE_MANAGER;
            }
            if (strcmp(f.key, "_dict_mgr") == 0) {
                g_ime.ensureUserDictLoaded();
                g_settingsState.mode = SETTINGS_DICT_CHOOSE;
                g_settingsState.dictSelection = 0;
                g_settingsState.dictScroll = 0;
                g_settingsState.dictSelected.clear();
                g_settingsState.dictSearchBuffer.clear();
                drawDictChoose();
                return APP_SETTINGS;
            }
            if (strcmp(f.key, "_bt_manage") == 0) {
                ctx.nextState = APP_BT_MANAGE;
                return APP_BT_MANAGE;
            }
            if (strcmp(f.key, "_cfg_backup") == 0) {
                startSettingsAsync(SettingsAsyncOp::Backup);
                return APP_SETTINGS;
            }
            if (strcmp(f.key, "_cfg_restore") == 0) {
                if (!settings_backup_exists()) {
                    // 卡不在和"卡在、但没有备份"是两件事，给的话得分开 —— 前者让用户去
                    // 插卡，后者去备份。
                    read_pico_sd_info_t sd = {};
                    const bool card = (read_pico_sd_get_info(&sd) == ESP_OK && sd.mounted);
                    ui_feedback_message(card ? "TF 卡上没有备份"
                                             : "未识别到 TF 卡，请插卡后重试");
                    return APP_SETTINGS;
                }
                // 覆盖当前设置是不可逆的（恢复**不删**备份里没有的文件，但被覆盖的那些
                // 旧值就没了），所以再点一次才动手。
                if (!s_cfgRestoreConfirm) {
                    s_cfgRestoreConfirm = true;
                    ui_feedback_message("恢复会覆盖当前设置，请再点一次确认");
                    return APP_SETTINGS;
                }
                s_cfgRestoreConfirm = false;
                startSettingsAsync(SettingsAsyncOp::Restore);
                return APP_SETTINGS;
            }
            if (strcmp(f.key, "_polish_prompt") == 0) {
                ctx.nextState = APP_POLISH_PROMPT;
                return APP_POLISH_PROMPT;
            }
            if (strcmp(f.key, "_click_try") == 0) {
                typingClickAudition(3);  // 只有一种音，试听就是把它放三声
                return APP_SETTINGS;
            }
        } else if (isToggleField(f.key)) {
            // 开/关切换:存 "1"(开) 或 "0"(关)
            bool next = !toggleValue(f.key);
            g_settings.setString(f.key, next ? "1" : "0");
            if (strcmp(f.key, "night_mode") == 0) {
                // 全设备夜间反色：走 board_set_night 一处转发（HalDisplay 标志 + display.c
                // 的实际取反点），再让快照失效 → 下一帧 ui_clear 重绘 + 整屏 GC16，立即看到效果。
                board_set_night(next);
                ui_invalidate_snapshot();
            } else if (strcmp(f.key, "click_enabled") == 0) {
                if (!next) typingClickRelease();
                else typingClickAudition(3);   // 打开就响一声，免得还要退出去试
                if (g_settingsState.selection > fieldVisibleCount() - 1)
                    g_settingsState.selection = fieldVisibleCount() - 1;
            }
        } else {
            g_settingsState.editBuffer = g_settings.getString(f.key);
            g_settingsState.editCursor = (int)g_settingsState.editBuffer.length();
            g_settingsState.editing = true;
            g_settingsState.imeActive = false;
            textSelReset();   // 换字段 → 上一个字段的选区作废
            editorVkAutoShow();   // 没连蓝牙键盘就弹虚拟键盘，否则这一页一个字也敲不进去
        }
    }

    // 列表重绘抽成函数：选项弹层要"列表照画、浮层盖在上面"，两处必须是同一张底图。
    drawBrowseList();
    return APP_SETTINGS;
}

// 模式方向那一档的四个值在列表行上怎么写（与 pickerOpts 的字面量同一套）。
// 两个方向项（计划模式方向 / 写作模式方向）共用：漏一处就会有一行**只剩标签、没有当前值**
// —— 字段行那条 if 链的兜底是 "▶ 名字"，看着像坏了。
static const char *modeOrientationLabel(const std::string &o) {
    if (o.empty()) return "跟随屏幕方向";
    if (o == "portrait") return "纵向";
    if (o == "landscape") return "横向";
    if (o == "auto") return "自适应";
    return o.c_str();   // 不认识的值原样显示（设置文件被手改过也别显示成空）
}

// 当前分类的字段列表（弹层的底图，也是浏览态的主画面）。
static void drawBrowseListBody() {
    ui_clear(); int y = FONT_H;
    std::string title = std::string("设置 · ") + SETTINGS_CAT_NAMES[s_cat];
    ui_draw_text_centered(y, title.c_str(), false, true); y += FONT_H;
    ListView lv = settingsBrowseListView();
    g_settingsState.selection = lv.sel;
    g_settingsState.scroll = lv.first;

    int rowCount = fieldVisibleCount();
    for (int i = 0; i < lv.rows && (lv.first + i) < rowCount; i++) {
        bool sel = (lv.first + i == lv.sel);
        int idx = fieldAt(lv.first + i); auto &f = SETTINGS_FIELDS[idx];
        char buf[80];
        if (f.action) {
            if (strcmp(f.key, "_font") == 0) {
                snprintf(buf, sizeof(buf), "▶ %s: %s", f.label, ttf_font_display_name());
            } else if (strcmp(f.key, "_polish_prompt") == 0) {
                snprintf(buf, sizeof(buf), "▶ %s: %s", f.label,
                         g_settings.polishPrompt().empty() ? "(未设置)" : "(已设置)");
            } else if (strcmp(f.key, "_app_mode") == 0) {
                snprintf(buf, sizeof(buf), "▶ %s: %s", f.label,
                         g_settings.appMode() == "quick" ? "快捷编辑" : "个人日记");
            } else if (strcmp(f.key, "_home_view") == 0) {
                snprintf(buf, sizeof(buf), "▶ %s: %s", f.label,
                         g_settings.homeView() == "month" ? "月视图" : "周视图");
            } else if (strcmp(f.key, "_orientation") == 0) {
                snprintf(buf, sizeof(buf), "▶ %s: %s", f.label,
                         g_settings.orientation() == "portrait" ? "纵向" : "横向");
            } else if (strcmp(f.key, "_gtd_orientation") == 0) {
                snprintf(buf, sizeof(buf), "▶ %s: %s", f.label,
                         modeOrientationLabel(g_settings.getString("gtd_orientation", "")));
            } else if (strcmp(f.key, "_writing_orientation") == 0) {
                snprintf(buf, sizeof(buf), "▶ %s: %s", f.label,
                         modeOrientationLabel(g_settings.getString("writing_orientation", "")));
            } else if (strcmp(f.key, "_editor_orientation") == 0) {
                snprintf(buf, sizeof(buf), "▶ %s: %s", f.label,
                         g_settings.editorOrientation() == "vertical" ? "竖排" : "横排");
            } else if (strcmp(f.key, "_clock_face") == 0) {
                snprintf(buf, sizeof(buf), "▶ %s: %s", f.label,
                         standbyFaceLabel(standbyFaceFromKey(g_settings.getString("clock_face").c_str())));
            } else if (strcmp(f.key, "_clock_preview") == 0) {
                snprintf(buf, sizeof(buf), "▶ %s", f.label);
            } else if (strcmp(f.key, "_cfg_backup") == 0 ||
                       strcmp(f.key, "_cfg_restore") == 0) {
                // 带出"卡上那份是什么时候的"，不然两个按钮一模一样，分不清有没有备份。
                const std::string stamp = settings_backup_stamp();
                snprintf(buf, sizeof(buf), "▶ %s: %s", f.label,
                         stamp.empty() ? "无备份" : stamp.c_str());
            } else if (strcmp(f.key, "_vertical_ref_line_style") == 0) {
                snprintf(buf, sizeof(buf), "▶ %s: %s", f.label,
                         VERTICAL_REF_LINE_STYLE_OPTS[verticalRefLineStyleIndex(g_settings.verticalReferenceLineStyle().c_str())].label);
            } else if (strcmp(f.key, "_input_mode") == 0) {
                snprintf(buf, sizeof(buf), "▶ %s: %s", f.label,
                         g_settings.inputMode() == "typewriter" ? "打字机模式" : "正常模式");
            } else if (strcmp(f.key, "_kb_layout") == 0) {
                snprintf(buf, sizeof(buf), "▶ %s: %s", f.label,
                         KB_LAYOUT_OPTS[kbLayoutIndex(g_settings.getString("kb_layout").c_str())].label);
            } else if (strcmp(f.key, "_ime_fuzzy") == 0) {
                snprintf(buf, sizeof(buf), "▶ %s: %s%s", f.label,
                         IME_FUZZY_OPTS[imeFuzzyIndex(g_settings.imeFuzzy().c_str())].label,
                         restartRequiredField(f.key) ? " 重启" : "");
            } else if (strcmp(f.key, "_ime_predict_mode") == 0) {
                snprintf(buf, sizeof(buf), "▶ %s: %s", f.label,
                         IME_PREDICT_OPTS[imePredictIndex(g_settings.imePredictMode().c_str())].label);
            } else if (strcmp(f.key, "_ime_clean") == 0) {
                snprintf(buf, sizeof(buf), "▶ %s: %s", f.label,
                         IME_CLEAN_OPTS[imeCleanIndex(g_settings.imeCleanMode().c_str())].label);
            } else if (strcmp(f.key, "_ime_commit_mode") == 0) {
                snprintf(buf, sizeof(buf), "▶ %s: %s", f.label,
                         IME_COMMIT_OPTS[imeCommitIndex(g_settings.imeCommitMode().c_str())].label);
            } else if (strcmp(f.key, "_ime_cand_size") == 0) {
                snprintf(buf, sizeof(buf), "▶ %s: %s", f.label,
                         IME_CAND_SIZE_OPTS[imeCandSizeIndex(g_settings.getString("ime_cand_size", "45").c_str())].label);
            } else if (strcmp(f.key, "_editor_font_size") == 0) {
                snprintf(buf, sizeof(buf), "▶ %s: %s", f.label,
                         EDITOR_FONT_SIZE_OPTS[editorFontSizeIndex(g_settings.getString("editor_font_size", "45").c_str())].label);
            } else if (strcmp(f.key, "_click_chinese") == 0) {
                snprintf(buf, sizeof(buf), "▶ %s: %s", f.label,
                         CLICK_CHINESE_OPTS[clickChineseIndex(g_settings.clickChineseMode().c_str())].label);
            } else if (strcmp(f.key, "_click_volume") == 0) {
                snprintf(buf, sizeof(buf), "▶ %s: %d%%", f.label, g_settings.typingClickVolume());
            } else {
                snprintf(buf, sizeof(buf), "▶ %s", f.label);
            }
        } else if (isToggleField(f.key)) {
            snprintf(buf, sizeof(buf), "%s:%s%s", f.label, toggleValue(f.key) ? "开" : "关",
                     restartRequiredField(f.key) ? " 重启" : "");
        } else {
            std::string value = g_settings.getString(f.key);
            std::string display;
            if (value.empty()) display = "(未设置)";
            else if (f.masked) display = "********";
            else display = value;
            snprintf(buf, sizeof(buf), "%s:%s", f.label, display.c_str());
        }
        ui_draw_text(8, y + i * FONT_H, buf, sel);
    }
}

// 浏览态主画面 = 列表 + 提交。弹层的底图只调 Body（提交由弹层自己那一次负责）。
static void drawBrowseList() {
    drawBrowseListBody();
    ui_commit();
}
