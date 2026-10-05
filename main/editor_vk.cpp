#include "editor_vk.h"

#include "font_renderer.h"
#include "icon_font.h"      // Shift/⌫ 键帽字形(NF-Propo 子集)
#include "pjournal_app.h"   // KEY_UP/DOWN/LEFT/RIGHT、KEY_SEARCH/HELP/CTRL_I/IME_TOGGLE…
#include "ui_helpers.h"
#include "u8g2_shim.h"
#include "hw/input.h"       // input_tap_peek_xy/input_tap_xy：软键盘点按泵要读点按落点
#include "hw/board.h"
#include "bt_keyboard.h"
#include "ime/IME.h"
#include "settings_manager.h"   // g_settings：键位布局（"kb_layout"）/候选字大小（"ime_cand_size"）
#include "ttf_font.h"           // ttf_draw_text_px / ttf_text_width_px / ttf_ascender_px

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// ── 字体：键盘用 g_vk_font，不跟"用户选的字体" ─────────────────────────────
// 界面文本一律走用户选的字体，**键盘除外**（用户要求把键盘字体固定下来）：键帽上的
// 字母、符号、汉字全用内置字体，键位/键宽/图标字形都不随设置漂移。g_vk_font 的文本面
// 钉在内置面上，格子模型仍是共享的那一份 —— 所以这里的 textWidth/ascent/descent
// 与 g_font 逐像素相等，把 g_font 换成 g_vk_font 对几何**零影响**。
// (候选行除外：那是输入法正文，编码串/候选字一直走 g_content_font，照旧跟用户字体。)
//
// ── 几何 ──────────────────────────────────────────────────────────────────
// 全部跟随当前字号(横竖屏通用)：面板高度 = 候选区 + 若干行按键 + 上下留白，
// 顶边贴住状态栏下方，正文区裁到面板顶边。
//
// 候选区分**两行**：上=编码，下=候选。原来挤在一行里，编码("ni" 这种)占掉左边
// 一截，候选只能从它右边开始排，横屏一行放不下几个词就得翻页；分行之后
// 候选独占整行宽度，正好等于输入法自己算页宽用的 imeCandidateLineWidth()
// (SCREEN_W-12)，页码和"一行放得下几个"终于对上了。代价是面板高了一行。
// ── 候选区两行的高度：随「候选字大小」设置浮动 ──────────────────────────
// 原来两行都固定 FONT_H+20（UI 字号下 65px），候选字跟界面同一档字号。现在候选字
// 可以单独调大/调小（设置项 ime_cand_size，单位=像素高，见 screen_settings 的
// 「候选字大小」）：候选行行高跟着候选字高走，面板顶边随之上下移，正文可视区跟着变。
//
// 编码行(行 0)仍按 UI 字号画，但行高跟候选行保持一致——两行不等高时那条分隔线会
// 看着歪，翻页起划的判定带也跟着变窄。
//
// **字号/度量/测宽/直绘**（imeCandFontPx/imeCandAscent/imeCandStrW/imeCandDrawText）
// 已经搬到 ui_helpers.cpp：实体键盘的输入法条也要按同一个候选字号画，两处必须逐像素
// 同一份（输入法按那份宽度分页）。留在本文件的只有**行高**——那是版面自己的事：键盘
// 面板要指尖点得着，行高给到 px+20；输入法条是贴身一条，px+8（见 imeBarRowH）。
// imeCandFontPx() 每次都现读设置：按键才重绘，几十分之一毫秒的 map 查找无所谓，
// 换来的是"改完设置立刻生效"，不必在屏间来回传状态。标准档 45 = 20pt 的 line_height，
// 与旧行为逐像素一致。
//
// 候选区两行的高度：
//   行 0（编码 / 拼音串）用**界面字号**画（g_content_font），所以它的下限是 FONT_H+20；
//   行 1（候选）跟着「候选字大小」走，px+20 —— 这块设置真正要改的就是这一行。
// 原来两行共用一个 max(px+20, FONT_H+20)，「小」档（34px）被下限顶住，改字号只缩字不缩
// 栏，看着就跟没生效一样。分行取高以后四档都跟得上：小 65+54、标准 65+65、
// 大 76+76、特大 88+88。
static int evkCandRowH(int row) {
    const int byFont = imeCandFontPx() + 20;
    if (row == 1) return byFont;      // 候选行：只跟候选字号，不受界面字号下限牽制
    const int minh = FONT_H + 20;     // 编码行：至少要放得下界面字号那一行字
    return byFont < minh ? minh : byFont;
}
static int evkCandH() { return evkCandRowH(0) + evkCandRowH(1); }   // 候选区总高
// 键行高：比字高高一截。指尖覆盖约 80px，行高只有 50px 时纵向几乎必然压到隔壁行，
// 这是"按 A 出 B"的主要来源。加高到 FONT_H+14（UI 字号下 59px）换取误触率下降，
// 代价是正文可视区少一行左右（面板顶边随之上移）。
#define EVK_ROW_H  (FONT_H + 14)
#define EVK_PAD    3

// 字母/符号三行再高一截：指尖纵向覆盖约 80px，行高不够时上下相邻行的判定带就窄，
// 一次误触要多刷一次屏、多退一次字。Ctrl 行与底部功能行只按一次、不参与连续输入，
// 保持原高——加高它们只会白挤正文可视行数。这一截直接换成触摸面积：字符键是唯一
// 连打的地方，宁可少一行正文也别让"按 A 出 B"。
#define EVK_LETTER_ROW_H (EVK_ROW_H + 26)

// 在 qwerty 之上固定一行「Ctrl | ◀ ▲ ▼ ▶ | 退格(靠右)」，退格因此从底部功能行
// 挪到这一行：方向键用来选字翻页和移光标，Ctrl 做组合键。横竖屏版式一致，只是
// keyW 不同（竖屏 68px、横屏 121px），所有尺寸都按 keyW 比例算。行下标由下面
// 三个函数统一给出，绘制与命中测试都走它们，避免两处几何算错位。
static int  evkLetter0()    { return 1; }              // 字母/符号首行下标
static int  evkFuncRow()    { return evkLetter0() + 3; }   // 底部功能行下标
static int  evkRowCount()   { return evkFuncRow() + 1; }

// 单行高度/行顶边：字母符号三行用加高后的行高，其余行用原高。
// 行顶边逐行累加（不能再用 row*EVK_ROW_H），绘制与命中测试共用。
static int  evkRowH(int row) {
    return (row >= evkLetter0() && row < evkFuncRow()) ? EVK_LETTER_ROW_H : EVK_ROW_H;
}
static int  evkRowY(int row) {
    int y = editorVkTop() + evkCandH() + EVK_PAD;
    for (int r = 0; r < row; r++) y += evkRowH(r);
    return y;
}
static int  evkPanelH() {
    int h = evkCandH() + 2 * EVK_PAD;
    for (int r = 0; r < evkRowCount(); r++) h += evkRowH(r);
    return h;
}

// 功能行五段：面板切换① | 面板切换② | 空格(图标) | 中/英 | 回车(图标,两格)。
// 一行恰好 10 格：①1格 ②1格 空格5格 中/英1格 回车2格。原来只有四段（123 | 中/英 |
// 空格 | 回车），空格独吃 5.7 格；现在把 [123] 拆成 [符] 和 [123] 两个键放到最左，
// 中/英从左端挪到回车左边（拇指够得着的地方），空格相应缩短到 5 格——仍然是一行里
// 最长的键，敲起来不至于变难点。
//
// ①②两个键是**面板切换**，标签随当前面板变（见 evkPanelAKeyLabel / evkPanelBKeyLabel）：
//   字母页 ①符 ②123   符号面板 ①ABC ②123   数字面板 ①符 ②ABC
// 三个面板正好绕一圈：任意一页按两下都能到另外两页，且都是从「回到字母页/去另一个
// 面板」这两个动作里挑，不存在"按了没反应"的键。①②的标签都是 1 个汉字或 3 个 ASCII
// （"ABC" 实测 66px @ 46px 字号），一格（竖屏 68px）装得下，不必再按内容取宽。
// 绘制与命中测试共用这一份几何。
static void evkFuncRowRects(int w, int keyW, int x[5], int kw[5]) {
    kw[0] = keyW;                                   // ①：符 / ABC
    kw[1] = keyW;                                   // ②：123 / ABC
    kw[2] = w - 5 * keyW;                           // 空格（吃剩余宽度，除不尽的余量也归它）
    kw[3] = keyW;                                   // 中/英
    kw[4] = 2 * keyW;                               // 回车
    if (kw[2] < keyW) kw[2] = keyW;                 // 极窄屏兜底：宁可挤也不重叠
    x[0] = 0;
    x[1] = kw[0];
    x[2] = x[1] + kw[1];
    x[3] = x[2] + kw[2];
    x[4] = w - kw[4];
}

// 额外行（qwerty 之上的那一行）的横向布局，八段共用一份几何，绘制与命中测试都走它：
//   [布局(2格)] [Ctl(1.5格)] [◀ ▲ ▼ ▶(各1格)] [Shift(1.25格)] [⌫(1.25格)]  = 恰 10 格
// 布局键要放得下"26键"两个字，2 格是下限；方向键那 4 格不动(箭头要方方正正的键面
// 才好看，缩了字形就没地方)。剩下 4 格在 Ctl / Shift / ⌫ 之间分：Ctl 只按一次、上面
// 就三个字母，从 2 格缩到 1.5 格足够；省下的半格连上原来各 1 格的 Shift/⌫，凑成
// 1.25 格——右侧这两个键原来又窄又只能塞个小图标，是整行里最难点中的两个。
// 用 keyW/4 的整数倍来算(Q=1/4 格)，避免浮点带来的绘制/命中小数差。
static void evkExtraRowRects(int w, int keyW, int x[8], int kw[8]) {
    const int q = keyW / 4;                          // 1/4 格
    x[0] = 0;            kw[0] = 8 * q;              // 布局切换(2 格)
    x[1] = 8 * q;        kw[1] = 6 * q;              // Ctl(1.5 格)
    for (int d = 0; d < 4; d++) { x[2 + d] = (14 + 4 * d) * q; kw[2 + d] = 4 * q; }  // 方向键
    x[6] = 30 * q;       kw[6] = 5 * q;              // Shift(1.25 格)
    x[7] = 35 * q;       kw[7] = w - x[7];           // ⌫(1.25 格 + w 除不尽的余量)
}

static bool s_visible = false;
static bool s_userOverride = false;   // 用户手动开关过 → 不再随蓝牙状态自动收起
static int  s_page = 0;               // 0=字母页 1=符号面板 2=数字面板（见 evkSymPage/evkNumPage）
static int  s_layout = IME::AMBIG_26; // 键位布局：26/14/18/9 键（见 IME::AmbigLayout）
static bool s_ctrl = false;           // Ctrl 待发：下一个普通键翻译成组合键后自动清除
static bool s_shift = false;          // Shift 待发：下一个字母转大写后自动清除
static bool s_t9 = false;             // T9 候选面板展开中(点编码行开关；组合结束自动收起)
static int  s_t9CodeTop = 0;          // 面板左列读音表滚到第几行(上下滑一次一行)

// 面板开合统一走这里。两个原因：一是收起时左列滚动位置必须归零（下次展开从第一个
// 读音看起）；二是面板一开就要把输入法切成宫格分页（每页 9 个，3×3 铺满），收起时
// 还原成候选条那套按实测宽度分页（单行只放得下 5~7 个）。分页规则跟着面板走，
// 两边不各记一份状态。
static void evkT9Set(bool on) {
    if (s_t9 == on) return;
    s_t9 = on;
    s_t9CodeTop = 0;
    IME::getInstance().setGridPaging(on);
}

// 刚按下的键（editorVkMarkPressed 记录，editorVkDraw 末尾补画成反色）。
// 不单独推屏：反馈搭按键动作自己那次刷新的顺风车。"抬起恢复"= 下一次重绘覆盖。
static EditorVkHit s_pressed;
static bool s_pressedValid = false;

// 字母页三行。
// 全部 ASCII：键表的 strlen 就是键数，混入 ¥ 等多字节字符会算错列数。
// 字母页第三行不走这张表：它固定 9 格满宽(zxcvbnm + ，。)，与 asdfghjkl 对齐。
static const char *EVK_LETTER_ROWS[3] = {"qwertyuiop", "asdfghjkl", "zxcvbnm"};

// 字母页第三行的七个字母，右侧接 ，。 两个标点键（共 9 格）。
static const char *EVK_LETTER_ROW3_MID = "zxcvbnm";

static const char *const *evkRows() { return EVK_LETTER_ROWS; }

// ── 符号面板 / 数字面板 ──────────────────────────────────────────────────
// 底部那排原来只有一个 [123] 键，按下去是**一张把数字和标点混在一起的三行表**
// （1234567890 / -/:;()$&@" / .,?!'"#%*+）。现在拆成两个各管一摊的面板，照手机上
// 输入法的两张面板来：数字单独成一张九宫格数字盘，标点单独成一张符号表。
//
// 符号面板就占**字母那三行**（行高、列宽、格数全不变，10 列 × 3 行），只是把第一行
// 的数字换成标点：数字既然有了自己的面板，再在符号表里占掉整整一行没道理。下面两行
// 保持原样不动——用了很久的键位挪位子比多一个符号烦人得多。
static const char *EVK_SYM_PANEL[3] = {"-/:;()$&@\"", ".,?!'\"#%*+", "~_^<>=[]{}|"};

static bool evkSymPage() { return s_page == 1; }
static bool evkNumPage() { return s_page == 2; }

// 数字面板用一个**四行**的版式，但它占的还是字母那三行的地方（"字母行带"）：
// 1..9 正好三行，加一行 0 就是四行 —— 手机上那张数字盘就是这么排的。行带总高不变
// （面板顶边、正文可视区都不动），只是把三行再均分成四行。
// 行高/行顶边逐行现算（不能写成 r*H），除不尽的余量落在最后一行，四行永远严丝合缝
// 地铺满整条行带。绘制与命中测试共用。
static int evkBandTop() { return evkRowY(evkLetter0()); }
static int evkBandH()   { return 3 * evkRowH(evkLetter0()); }
static int evkNumRowY(int r) { return evkBandTop() + evkBandH() * r / 4; }
static int evkNumRowH(int r) { return evkBandTop() + evkBandH() * (r + 1) / 4 - evkNumRowY(r); }

// 数字面板的列：左列(# * - +) | 中间三列(1..9，末行 0 横跨三列) | 右列(⌫ . @ /)。
// 左右两列各 1.5 格 —— 标签都是单个 ASCII 字符，一格就够，但数字盘两边习惯上留宽一点
// 的手指落点（照手机的比例）。中间三列均分剩下的宽度，除不尽的余量归最后一列。
// 左右两列各四格，和中间四行一一对齐（手机上那排 # * - + 也是四格对三行）。
static int evkNumSideW(int w) { return w * 15 / 100; }

// 数字面板左列的四个符号（行 0..3）与右列四个键：右列第 0 格是退格图标键（shape 4），
// 其余是文字键。左列全是文字键。
static const char EVK_NUM_LEFT[4] = {'#', '*', '-', '+'};
static const char EVK_NUM_RIGHT[4] = {0, '.', '@', '/'};   // [0] 占位：退格是图标键

// ── 一键多字母布局（14 / 18 / 9 键）──────────────────────────────────────
// 一行一组键，每组是 1-4 个字母；键上显示的标签就是这组字母（如 "qw"、"2 abc"），
// 用户按 QWERTY 或九宫格的位置记忆即可。三行都撑满屏宽，组宽见 evkAmbigRowRects。
// **分组表不在这里**：组号、字母、标签、行布局一律取自 IME（见 IME::ambigRowGroup
// 与 IME::AmbigLayout），阅读模式的键盘读的是同一份，两个键盘不会走样。

// 一键多字母布局某一行的横向布局（绘制与命中测试共用）。三行**都撑满整个屏宽（10 格）**，
// 和 qwerty 第一行同宽：原来第二、三行只排 9 格 / 7 格，右端空着一截，`l` 和 `m`
// 分别只有 1 格宽（横屏 121px），比旁边的两字母键窄一半，整行看着也缺一角。
// 现在一行里的 n 个组**等宽**平分 10 格，组宽只跟"这一行有几个组"有关：
//   14 键 行0 qw er ty ui op → 5 组 × 2 格    行1 as df gh jk l → 5 组 × 2 格
//         行2 zx cv bn m      → 4 组 × 2.5 格（m 从 1 格长到 2.5 格）
//   18 键 行0 7 组 × 1.43 格   行1 6 组 × 1.67 格   行2 5 组 × 2 格
//   9  键 三行都按 3 列定宽（3.33 格），末行 2 个键居中 —— 九宫格的样子
// 不再按字母数比例分配：`l`/`m` 这种单字母组会被比例压到最窄，正是要修的地方；
// 组里有几个字母已经写在键面上了，键宽不必再编码一次。
// 除不尽的余量落在最后一个键（见下），所以行宽永远是整 10 格、不重叠。
static void evkAmbigRowRects(int r, int w, int keyW, int *xs, int *ws) {
    int n = IME::ambigRowKeys(r);
    const int cols = IME::ambigRowCols(r);
    if (n <= 0 || cols <= 0) return;
    const int span = 10 * keyW;
    if (n == cols) {
        // 整行撑满：n 组等分 10 格，除不尽的余量落在最后一格（见上面的说明）。
        const int x0 = (w - span) / 2;
        for (int k = 0; k < n; k++) {
            int a = x0 + span * k / n;
            int b = x0 + span * (k + 1) / n;
            xs[k] = a;
            ws[k] = b - a;
        }
        return;
    }
    // 九宫格（9 键）：键宽按 cols 列定，一行不满就整行居中 —— 末行的 2 个键与上面
    // 三列一一对齐，而不是拉成两条满宽的大长键。
    const int cell = span / cols;
    const int x0 = (w - cell * n) / 2;
    for (int k = 0; k < n; k++) {
        xs[k] = x0 + k * cell;
        ws[k] = cell;
    }
}

// ── 九宫格（9 键）专用几何 ────────────────────────────────────────────────
// 版式照手机 T9：左边一列，中间 3×3 九宫格（1 分词 + 2~9 字母），右边一列功能键
// （⌫ / 重输 / 0）——退格因此从上面那行挪到了手指最顺的右侧，和手机上同一个位置。
// 左列是**双身份**的：没有组合时是四个标点键（，。？！），有组合时换成**分音节选择
// 列**——和点开编码行之后的 T9 面板左列同一份内容、同一套算法（见 evkT9*），见
// evkNineSylOn / evkNineSylColDraw。
// 宽度不按格分：左列宽度由 evkNineLeftW 定（要放得下最长的拼音音节），右列 1.6 格
// ——"重输"两个字在竖屏下就要 100px（整屏才 684px 宽），1.0 格（68px）装不下，标签
// 会顶出键框线；手机上的右列也宽于左列，比例大致相当。仨字母列均分剩下的。
// 绘制与命中测试都从这一份算，不会各算一遍慢慢错位。
static int evkNineLeftW(int w) {
    // 音节最长 6 个字母（zhuang/chuang/shuang），实测 150px；+20 是两边留白和
    // 右侧那条滚动条的位置（照抄 evkT9LeftW 的算法，只是这里把上限写成真实的最长音节，
    // 免得宽度随"这一轮还剩哪几个音节"上下跳——列宽一跳，整片键位跟着跳）。
    const int lo = w / 10;                            // 至少一格，标点键不至于太细
    const int syl = g_vk_font.textWidth("zhuang") + 20;
    return syl > lo ? syl : lo;
}

static void evkNineCols(int w, int *punctW, int *colX, int *colW, int *rightX, int *rightW) {
    const int pw = evkNineLeftW(w);
    const int rw = w * 16 / 100;
    const int cw = (w - pw - rw) / 3;
    *punctW = pw;
    *colX = pw;
    *colW = cw;
    *rightX = pw + cw * 3;
    *rightW = w - *rightX;   // 吃下除不尽的余量，右列正好贴住右边
}

// 左列第 i 个标点键（0..3）在键区里的 y / 高：把三个字母行的高度**均分成四格**。
// 手机上那条标点列就是这么排的——和九宫格齐高，四个小键平分。
static void evkNinePunctCell(int gridY, int gridH, int i, int *y, int *h) {
    const int a = gridY + gridH * i / 4;
    const int b = gridY + gridH * (i + 1) / 4;
    *y = a;
    *h = b - a;
}

// 九宫格模式下额外行的横向布局：格数与 evkExtraRowRects 相同，只少了最右边的 ⌫
// （它已经挪进九宫格右列），腾出来的 1.25 格并给 Shift——它原来只有 1.25 格，是整行
// 里最难点中的键之一。
//   [布局 2 格] [Ctl 1.5 格] [◀ ▲ ▼ ▶ 各 1 格] [Shift 2.5 格] = 恰 10 格
static void evkNineExtraRowRects(int w, int keyW, int x[7], int kw[7]) {
    const int q = keyW / 4;
    x[0] = 0;                       kw[0] = 8 * q;   // 布局切换
    x[1] = 8 * q;                   kw[1] = 6 * q;   // Ctl
    for (int d = 0; d < 4; d++) { x[2 + d] = (14 + 4 * d) * q; kw[2 + d] = 4 * q; }
    x[6] = 30 * q;                  kw[6] = w - x[6];   // Shift 吃掉 ⌫ 让出的那格
}

// 中文标点态：输入法开着且不是英文态 —— 此时「，。」按全角上屏(IME 的
// handleFullwidthPunct 自己会把半角 ',' '.' 换成全角)，标签也跟着变。
static bool evkCnPunct() {
    IME &ime = IME::getInstance();
    return ime.active() && !ime.english();
}

// 现在画的是不是"一键多字母"布局（14/18/9 键之一）。**英文态(含没开输入法)一律回到
// 26 键**：这类布局的键面是 "qw""we""abc" 这种组合标签，只有输入法开着中文时才有
// 意义——英文态下它只能取组首字母(见命中测试)，标签就是错的，白占一行让人以为能打
// 全拼。s_layout 本身保留着用户的选择，切回中文还是那一套。
static bool evkAmbig() { return s_layout != IME::AMBIG_26 && !s_page && evkCnPunct(); }

// 现在画的是不是九宫格（9 键）。九宫格有一整套自己的版式（左标点列 + 3×3 + 右功能列，
// 见 evkNineCols），不走 evkAmbigRowRects 那套"每行撑满、末行居中"的算法。
static bool evkNineKey() { return evkAmbig() && s_layout == IME::AMBIG_9; }

// 九宫格左列此刻该画什么：组合中且真还有可挑的音节 → 分音节选择列；否则 → 标点键。
// 两个条件缺一不可——ambigActive() 保证这张表是**当前**这一轮算出来的（见 IME.h），
// 非空则保证有东西可画（剩下的键拼不出完整音节时它会是空的，这时画一行空列不如
// 退回标点，用户还能打个逗号）。
static bool evkNineSylOn() {
    IME &ime = IME::getInstance();
    return ime.ambigActive() && !ime.ambigSyllables().empty();
}

// 九宫格字母键的键面文字。正常是 IME 的组标签 "9 WXYZ"；列太窄放不下时去掉中间那个
// 空格变成 "9WXYZ"——竖屏下字母列只剩 135px，带空格的标签要 150px，会顶出键框线。
// 「1 分词」同理（g < 0）。绘制和按下反馈必须用同一个标签（evkSetHit 回带的是同一份
// 文字），所以只此一处算，别处都来问它。
static const char *evkNineKeyLabel(int g, int colW, char *buf, size_t cap) {
    const char *lab = (g < 0) ? "1 分词" : IME::ambigGroupLabel(g);
    if (g_vk_font.textWidth(lab) + 8 <= colW) return lab;
    size_t n = 0;
    for (const char *p = lab; *p && n + 1 < cap; p++) {
        if (*p == ' ') continue;   // 只挤掉那个分隔空格，别的都留着
        buf[n++] = *p;
    }
    buf[n] = 0;
    return buf;
}

// 布局键的键面文字（带"键"字，给人看）；四个布局循环，一眼看得出现在是哪一个。
static const char *evkLayoutLabel(int layout) {
    static const char *const N[] = {"26键", "14键", "18键", "9键"};
    if (layout < 0 || layout >= (int)(sizeof(N) / sizeof(N[0]))) return N[0];
    return N[layout];
}

// 键面上**真正生效**的那个布局的标签。s_layout 是用户的选择，但只要 evkAmbig() 为假
// （输入法没开 / 英文态），画出来的就是 26 键键位 —— 这时标签必须跟着说"26键"，
// 否则就是"写着 14键、画着 26键"（用户报障：阅读模式的虚拟键盘，标签与他选的布局
// 不符，按一下才跳过去）。绘制与按下反馈共用同一个标签，所以只此一处算。
static const char *evkLayoutLabelEff() {
    return evkLayoutLabel(evkAmbig() ? s_layout : IME::AMBIG_26);
}

// 布局的存储键名（不带"键"，就是设置项 "kb_layout" 里存的那串字），与
// evkLayoutLabel 一一对应。持久化用这一套。
static const char *evkLayoutKey(int layout) {
    static const char *const K[] = {"26", "14", "18", "9"};
    if (layout < 0 || layout >= (int)(sizeof(K) / sizeof(K[0]))) return K[0];
    return K[layout];
}

static int evkLayoutFromKey(const char *key) {
    if (!key) return IME::AMBIG_26;
    for (int i = 0; i < IME::ambigLayoutCount(); i++)
        if (strcmp(key, evkLayoutKey(i)) == 0) return i;
    return IME::AMBIG_26;
}

// 换布局：UI 的 s_layout 是唯一真相，IME 那边跟着走（组号→字母/标签都从它取）。
// 不写设置——持久化只在用户主动改（点布局键 / 设置项）时发生。
static void evkApplyLayout(int layout) {
    if (layout < 0 || layout >= IME::ambigLayoutCount()) layout = IME::AMBIG_26;
    s_layout = layout;
    IME::setAmbigLayout(layout);
}

// 外部入口（设置项 / 开机恢复）。key = "26"/"14"/"18"/"9"；写设置项，下次开机还是它。
void editorVkSetLayout(const char *key) {
    evkApplyLayout(evkLayoutFromKey(key));
    g_settings.setString("kb_layout", evkLayoutKey(s_layout));
}

const char *editorVkLayoutKey() { return evkLayoutKey(s_layout); }

// 符号页第三行是 10 个 ASCII 标点，满宽，和第一行(qwertyuiop)左右对齐，只多出
// SCREEN_W%10 的余量居中。绘制与命中测试共用此值。
static int evkRow3X(int w, int keyW) { return (w - 10 * keyW) / 2; }

// ── 候选区两行 ───────────────────────────────────────────────────────────
// 行 0 = 编码(拼音串/英文词)，行 1 = 候选。两行各自取高（见 evkCandRowH），所以第 1 行的
// y 是"行 0 的高"，不能再写成 row * h。
static int evkCandRowY(int row)   { return editorVkTop() + (row == 0 ? 0 : evkCandRowH(0)); }
// 行内基线：候选行按候选字自己的像素高居中（ascent 也取那个字号），编码行仍是 UI 字号。
static int evkCandTextBaseline(int row) {
    const int rh = evkCandRowH(row);
    if (row == 1) return evkCandRowY(row) + (rh - imeCandFontPx()) / 2 + imeCandAscent();
    return evkCandRowY(row) + (rh + g_vk_font.ascent() - g_vk_font.descent()) / 2;
}
// 反白块(高 = 候选字高)在行内的 y。候选行用它；编码行不用。
static int evkCandHlY(int row)    { return evkCandRowY(row) + (evkCandRowH(row) - imeCandFontPx()) / 2; }
static int evkCandHlH()           { return imeCandFontPx(); }

// 候选串的字号/测宽/直绘全在 ui_helpers.cpp（imeCandFontPx / imeCandStrW /
// imeCandDrawText）：实体键盘的输入法条要按同一个字号画同一串字，两处只能有一份。
// 这里只留一行别名，免得下面几十处调用点全改名。
static int evkCandStrW(const char *s) { return imeCandStrW(s); }
static void evkCandDrawText(int x, int baseline, const char *s, bool invert) {
    imeCandDrawText(x, baseline, s, invert);
}

// 候选行布局缓存(绘制与命中测试共用)。
static std::vector<std::string> s_candLabel;
static std::vector<int> s_candX;
static std::vector<int> s_candW;

// 候选从 x=6 起排满整行(编码已经挪到上一行，这里不再给它让位)：可用宽度是
// SCREEN_W-10，和输入法分页时假设的 imeCandidateLineWidth()(SCREEN_W-12) 对得上，
// 所以"这一页放得下几个"两边算的是同一回事，不会出现分页留了空位却排不下的情况。
static void evkComputeCandLayout() {
    s_candLabel.clear();
    s_candX.clear();
    s_candW.clear();
    IME &ime = IME::getInstance();
    int x = 6;
    const auto &cands = ime.candidates();
    int ps = ime.pageSize();
    for (size_t i = 0; i < cands.size(); i++) {
        std::string lab = std::to_string(static_cast<int>(i % ps) + 1) + "." + cands[i];
        int wpx = evkCandStrW(lab.c_str()) + 12;
        if (x + wpx > SCREEN_W - 4) break;   // 放不下就截断，不挤到屏幕外
        s_candLabel.push_back(lab);
        s_candX.push_back(x);
        s_candW.push_back(wpx);
        x += wpx;
    }
}

// 键帽上的图标字形：字形按包围盒 contain 缩放居中(icon_font_draw_sized 的语义)。
// 框**可以不是方的**：空格字形本身是 3:1 的长条，塞进正方形框只能按宽缩放、高度
// 白白浪费一半；给它一个扁框，笔画才铺得开。框在键面内居中，四边各留一点。
// 图标字体直写帧缓冲(和 FontRenderer 走同一条路)，不经过 shim 的绘制颜色，
// invert 自己按反白态决定：fill 过的键底是黑的，字形就得画白的。
// bw/bh = 想要的框；0 = 按键高的 78%(默认给得比较满，图标太小在 e-ink 上认不出)。
static void evkIconKeyGlyph(int x, int y, int w, int h, uint32_t cp, bool filled, int bw = 0,
                            int bh = 0) {
    if (bw <= 0) bw = h * 78 / 100;
    if (bh <= 0) bh = bw;
    if (bw > w - 6) bw = w - 6;         // 框内切在键面里，别越到键框线上
    if (bh > h - 6) bh = h - 6;
    if (bw > 64) bw = 64;               // ICON_MAX_PX 是 72，留点余量
    if (bh > 64) bh = 64;
    if (bw < 8 || bh < 8) return;
    icon_font_draw_sized(u8g2_GetBufferPtr(g_u8g2), x + (w - bw) / 2, y + (h - bh) / 2, bw, bh, cp,
                         filled, 0);
}

// 方向键字形(NF-Propo 的 Material 实心箭头)。dir: 0=左 1=上 2=下 3=右。
// 和 Shift/⌫ 一样走图标字体：手画的三角是"逐行短横拼出来"的，笔画粗细随边长变化，
// 四支箭头在小键帽上粗细还不一致；字形自带正确的轮廓，放大缩小都匀。
// 顺序必须与 EditorVkHit.dir 以及 DIRKEY[] 的下标一致(左/上/下/右)。
static uint32_t evkDirGlyph(int dir) {
    static const uint32_t kCp[4] = {0xF004E, 0xF005E, 0xF0046, 0xF0055};
    return (dir >= 0 && dir < 4) ? kCp[dir] : 0;
}

// 方向键：外框 + (可选)反白填充 + 居中箭头字形。
// filled：反白态（按下反馈/待发），键底填实，字形改用白色画，否则糊成一片黑。
static void evkTriangleKey(int x, int y, int w, int h, int dir, bool filled = false) {
    u8g2_SetDrawColor(g_u8g2, 0);
    u8g2_DrawFrame(g_u8g2, x, y, w, h);
    if (filled) u8g2_DrawBox(g_u8g2, x + 1, y + 1, w - 2, h - 2);
    u8g2_SetDrawColor(g_u8g2, 1);
    evkIconKeyGlyph(x, y, w, h, evkDirGlyph(dir), filled, h - 14);   // 箭头几乎顶满键帽
}

// Shift 键帽：外框 + NF-Propo 的 Shift 记号(U+F0069，实心上箭头压一条底线)。
// 之前是手画的上三角＋短横，笔画粗细和字形对不上；现在直接用图标字形，和正文
// 里的图标同一套渲染。字体子集里没有 U+21E7(⇧)，就只能走这个码点。
// 字形是 1.35:1 的宽扁形，方框里按宽缩放，所以框给满一点才够高（原来 55% 的框
// 画出来只有 26px 高，比旁边的方向键小一大截）。
static void evkShiftKey(int x, int y, int w, int h, bool filled) {
    u8g2_SetDrawColor(g_u8g2, 0);
    u8g2_DrawFrame(g_u8g2, x, y, w, h);
    if (filled) u8g2_DrawBox(g_u8g2, x + 1, y + 1, w - 2, h - 2);
    u8g2_SetDrawColor(g_u8g2, 1);
    evkIconKeyGlyph(x, y, w, h, 0xF0069, filled);
}

// ⌫ 键帽：外框 + NF-Propo 的退格记号(U+F006E，标签形里一个叉)。原来手画的叉号
// 在视觉上就是"关闭"而不是"退格"；这个字形本身带键盘上的退格轮廓，一眼能认。
// 和 Shift 一样放宽了字形框（1.33:1 的宽扁形，同样按宽缩放）。
static void evkBackspaceKey(int x, int y, int w, int h, bool filled) {
    u8g2_SetDrawColor(g_u8g2, 0);
    u8g2_DrawFrame(g_u8g2, x, y, w, h);
    if (filled) u8g2_DrawBox(g_u8g2, x + 1, y + 1, w - 2, h - 2);
    u8g2_SetDrawColor(g_u8g2, 1);
    evkIconKeyGlyph(x, y, w, h, 0xF006E, filled);
}

// 空格键帽：外框 + NF-Propo 的键盘空格记号(U+F1050，一个下开口的宽 U)。
// 原来键面写"空格"两个字，占掉半个键宽不说，和同一行里图标化的 Shift/⌫ 也不搭。
// 字形是 3:1 的长条：方框里只能画到 50px 宽，用宽扁框(键宽减两边的留白)才能
// 画成一条像样的空格条。高度上限仍是键高减 6，所以实际是"框多宽、条就多宽"。
static void evkSpaceKey(int x, int y, int w, int h, bool filled) {
    u8g2_SetDrawColor(g_u8g2, 0);
    u8g2_DrawFrame(g_u8g2, x, y, w, h);
    if (filled) u8g2_DrawBox(g_u8g2, x + 1, y + 1, w - 2, h - 2);
    u8g2_SetDrawColor(g_u8g2, 1);
    int bw = w - 24;
    if (bw > 240) bw = 240;             // 键再宽，空格条也别拉成一条线
    evkIconKeyGlyph(x, y, w, h, 0xF1050, filled, bw, h - 6);
}

// 回车键帽：外框 + NF-Propo 的键盘回车记号(U+F0311，左弯的 ↵)。同"空格"，
// 原来写"回车"两个字(100px 放不下一格，键才给的两格宽)，现在换成图标。
// 字形 1.58:1，方框里按宽缩放，框给满。
static void evkReturnKey(int x, int y, int w, int h, bool filled) {
    u8g2_SetDrawColor(g_u8g2, 0);
    u8g2_DrawFrame(g_u8g2, x, y, w, h);
    if (filled) u8g2_DrawBox(g_u8g2, x + 1, y + 1, w - 2, h - 2);
    u8g2_SetDrawColor(g_u8g2, 1);
    evkIconKeyGlyph(x, y, w, h, 0xF0311, filled);
}

// 单个键：外框 + (可选)反白填充 + 居中标签。
static void evkKey(int x, int y, int w, int h, const char *label, bool filled) {
    u8g2_SetDrawColor(g_u8g2, 0);
    u8g2_DrawFrame(g_u8g2, x, y, w, h);
    if (filled) u8g2_DrawBox(g_u8g2, x + 1, y + 1, w - 2, h - 2);
    int tw = g_vk_font.textWidth(label);
    int bx = x + (w - tw) / 2;
    int by = y + (h + g_vk_font.ascent() - g_vk_font.descent()) / 2;
    g_vk_font.drawText(bx, by, label, filled);
    u8g2_SetDrawColor(g_u8g2, 1);
}

// ── 防误触：中心优先的键命中 ────────────────────────────────────────────
// 键面画的是矩形，命中区取它的**内切椭圆**：点必须落在椭圆内才算按到这个键。
// 四个角因此成为死区——相邻键的角彼此靠近，手指落在两键之间的对角地带（最容易
// 误触的位置）时会被吞掉，而不是"就近猜一个键"。误触在 e-ink 上代价偏大：多一次
// 刷屏，还得多退一次字。
static bool evkInEllipse(int x, int y, int kx, int ky, int kw, int kh) {
    if (kw <= 0 || kh <= 0) return false;
    const double dx = (x - (kx + kw / 2.0)) / (kw / 2.0);
    const double dy = (y - (ky + kh / 2.0)) / (kh / 2.0);
    return dx * dx + dy * dy <= 1.0;
}

// Ctrl 待发时把普通键翻译成组合键，翻译完自动清除。
// 字母走标准控制码 0x01..0x1A，但 i/空格/回车/斜杠/问号在 pjournal 里有专用键码
// (KEY_CTRL_I 灵感面板、KEY_IME_TOGGLE 输入法开关、KEY_CTRL_ENTER、KEY_SEARCH
// 查找替换、KEY_HELP 快捷键帮助)，必须按专用码返回，否则会被当成 Tab 之类。
static int evkFinishKey(int k) {
    // 翻页/中英/Ctrl/Shift/布局切换自身不是文字输入，不参与组合也不消耗待发的修饰键。
    if (k == EVK_NONE || k == EVK_PAGE || k == EVK_LANG || k == EVK_CTRL || k == EVK_SHIFT ||
        k == EVK_LAYOUT) return k;
    // 一键多字母的组码同样不参与 Ctrl/Shift 翻译（它按下一个字母组，不是字母）。
    if (k >= IME::KEY_AMBIG_BASE) return k;
    if (s_shift && k >= 'a' && k <= 'z') {          // Shift 待发只吃字母，其余键不动它
        s_shift = false;
        k = k - 'a' + 'A';
    }
    if (!s_ctrl) return k;
    s_ctrl = false;
    if (k == '?') return KEY_HELP;
    if (k == '/') return KEY_SEARCH;
    if (k == ' ') return KEY_IME_TOGGLE;
    if (k == '\n') return KEY_CTRL_ENTER;
    if (k >= '0' && k <= '9') return KEY_FILE_BASE + (k - '0');   // Ctrl+0-9 快捷编辑文件
    if (k >= 'a' && k <= 'z') return k == 'i' ? KEY_CTRL_I : (k - 'a' + 1);
    if (k >= 'A' && k <= 'Z') return k == 'I' ? KEY_CTRL_I : (k - 'A' + 1);
    return k;   // 其余(退格/标点)按原键处理
}

// ── 对外接口 ──────────────────────────────────────────────────────────────

// 从设置里把上次选定的键位布局读回来并应用。
// 之前这段只在 editorVkInit 里做，而 editorVkInit 只被写作/计划编辑器进入时调用——
// 于是"开机后一直只用阅读模式"的用户，直到重启都没能拿回自己上次选的布局：s_layout
// 停在静态默认值 26。把读取提出来，交给"键盘每次展开"的路径也调一次。
// 幂等：改布局处（editorVkSetLayout / 键盘上的布局键）都会同步写回设置项，所以
// s_layout 与 "kb_layout" 始终一致，重复读等价于没读。
static void evkLoadLayout() {
    evkApplyLayout(evkLayoutFromKey(g_settings.getString("kb_layout").c_str()));
}

void editorVkInit() {
    s_visible = !g_bt.isConnected();
    s_userOverride = false;
    s_page = 0;
    s_ctrl = false;
    s_shift = false;
    // 键位布局从设置恢复（不写回：这条路径每次进编辑器都会走，别把读变成写）。
    evkLoadLayout();
    evkT9Set(false);
    s_pressedValid = false;
}

void editorVkSyncBtState() {
    if (!s_userOverride && s_visible && g_bt.isConnected()) {
        s_visible = false;
        evkT9Set(false);
        s_pressedValid = false;
    }
}

void editorVkAutoShow() {
    if (g_bt.isConnected()) return;   // 有物理键盘就别挡屏幕
    evkLoadLayout();                  // 顺手把上次选的键位布局读回来（幂等）
    s_visible = true;
    s_pressedValid = false;
    s_userOverride = false;           // 恢复"跟随蓝牙状态自动收起"
    s_ctrl = false;
    s_shift = false;
    evkT9Set(false);                  // 新一次输入会话从键盘开始，不是从候选面板开始
}

bool editorVkVisible() { return s_visible; }

void editorVkSetVisible(bool on) {
    if (s_visible != on)
        IME::getInstance().invalidateCandidateWidths();   // 换人画候选 → 换量法，旧宽度作废
    s_visible = on;
    s_userOverride = true;
    s_ctrl = false;   // 收起/展开时丢弃待发的修饰键，避免隔一次开关后意外生效
    s_shift = false;
    if (on) {
        // 展开键盘时把上次选的布局读回来。阅读模式从不调 editorVkInit，靠这一条
        // 才能拿回自己的布局（详见 evkLoadLayout 的注释）。
        evkLoadLayout();
    } else {
        s_pressedValid = false;   // 收起后不留"某个键还按着"的反色
        evkT9Set(false);          // 面板也跟着收起，展开时不至于突然顶着一张宫格
    }
}

void editorVkAutoHide() {
    if (s_visible) IME::getInstance().invalidateCandidateWidths();  // 同 editorVkSetVisible
    s_visible = false;
    s_userOverride = false;   // 界面自己收的，不算"用户手动关过"
    s_ctrl = false;
    s_shift = false;
    s_pressedValid = false;
    evkT9Set(false);
}

bool editorVkPumpTap(bool imeActive, int *outKey, bool *outTurnImeOn) {
    if (outKey) *outKey = 0;
    if (outTurnImeOn) *outTurnImeOn = false;
    int x = 0, y = 0;
    if (!input_tap_peek_xy(&x, &y)) return false;
    if (!s_visible || y < editorVkTop()) return false;   // 没落在面板上：原样留给界面
    input_tap_xy(&x, &y);                                // 归键盘了，消费掉

    EditorVkHit hit;
    int k = editorVkHitTest(x, y, &hit);
    if (k != EVK_NONE) editorVkMarkPressed(hit);
    if (k == EVK_PAGE) {
        // 面板已在命中测试里切好，这里什么都不用做：返回 *outKey = 0，调用方重绘即可。
    } else if (k == EVK_LANG) {
        // 未开输入法 → 开中文；已开 → 拼音/英文互切。开输入法这一步得调用方自己做
        // （它的 imeActive 是界面私有状态），这里只把"该开"回报出去。
        if (!imeActive) { if (outTurnImeOn) *outTurnImeOn = true; }
        else IME::getInstance().toggleEnglish();
    } else if (k > 0) {
        if (outKey) *outKey = k;
    }
    // EVK_CTRL/EVK_SHIFT/EVK_T9/EVK_LAYOUT/EVK_SEP/EVK_CLEAR/EVK_NOOP：动作已在命中
    // 测试内就地完成，返回 *outKey = 0，调用方重绘即可把结果刷出去。
    return true;
}

int editorVkTop() {
    // 键盘面板是**界面框架**：这一族函数会被编辑器正文作用域调到（算正文底边、命中
    // 键盘区），几何恒按界面字号，不跟正文字号一起长。见 screen_editor_handle 的说明。
    FontScope ui(FontRenderer::uiPxHeight());
    return STATUS_BAR_Y - evkPanelH();
}
// 候选区总高：同样是**界面框架**几何（evkCandRowH 里有 FONT_H + 20 的下限），
// 必须自己钉回界面字号 —— 调用它的是 ui_render 的候选区矩形和阅读器的
// displayBufferVk，两处都不在编辑器正文作用域里，不钉就会拿正文字号算出一个跟
// editorVkTop()（已钉）不配套的高度。
int editorVkCandH() { FontScope ui(FontRenderer::uiPxHeight()); return evkCandH(); }

int editorVkIconSlotW() { FontScope ui(FontRenderer::uiPxHeight()); return FONT_H; }

bool editorVkIconHit(int x, int y) {
    FontScope ui(FontRenderer::uiPxHeight());
    int slot = editorVkIconSlotW();
    return slot > 0 && y >= STATUS_BAR_Y && x >= SCREEN_W - slot;
}

// 把命中的键按反色重画一次（按下态的视觉反馈）。只由 editorVkDraw 最后调用。
static void evkDrawPressedInv(const EditorVkHit &hit) {
    const bool inv = !hit.filled;   // 按下 = 常态取反
    switch (hit.shape) {
        case 1: evkTriangleKey(hit.x, hit.y, hit.w, hit.h, hit.dir, inv); break;
        case 2: evkShiftKey(hit.x, hit.y, hit.w, hit.h, inv); break;
        case 4: evkBackspaceKey(hit.x, hit.y, hit.w, hit.h, inv); break;
        case 5: evkSpaceKey(hit.x, hit.y, hit.w, hit.h, inv); break;
        case 6: evkReturnKey(hit.x, hit.y, hit.w, hit.h, inv); break;
        case 3: {
            u8g2_SetDrawColor(g_u8g2, inv ? 0 : 1);
            u8g2_DrawBox(g_u8g2, hit.x, hit.y, hit.w, hit.h);
            u8g2_SetDrawColor(g_u8g2, 1);
            int tw = g_vk_font.textWidth(hit.text);
            int bx = hit.x + (hit.w - tw) / 2;
            int by = hit.y + (hit.h + g_vk_font.ascent() - g_vk_font.descent()) / 2;
            g_vk_font.drawText(bx, by, hit.text, inv);
            break;
        }
        default: evkKey(hit.x, hit.y, hit.w, hit.h, hit.text, inv); break;
    }
}

// T9 候选面板(实现在命中测试前，与它共用几何)：绘制入口与"是否展开"。
static bool evkT9Active();
static void evkT9Draw(int w);

// 两张面板的命中（实现排在 editorVkHitTest 之后：它们要用 evkFinishKey/evkSetHit，
// 那两件就在下面一点，而绘制那半边已经先用到面板几何了）。绘制走 evkSymPanelDraw /
// evkNumPanelDraw（排在 editorVkDraw 之前），命中走这两个。
static int evkSymPanelHit(int x, int y, int w, EditorVkHit *hit);
static int evkNumPanelHit(int x, int y, int w, EditorVkHit *hit);

// 九宫格左列在"组合中"的那一面（实现在 T9 面板那套几何之后——用的是同一组函数
// evkT9CodeRowH/Vis/First，那份几何在文件里排得靠后）。绘制、命中、上下滑三处都要
// 这份窗口，所以由 evkNineSylWindow 一处算完再各自用。
static void evkNineSylColDraw(int lw, int gy, int gh);
static int evkNineSylWindow(int gh, int *n, int *rh, int *vis, int *first, int *last);

// 九宫格键区（只给 9 键布局用，见 evkNineCols）。hLet = 字母行高，三行同高。
// 上屏的键面标签：字母键用 IME 的组标签（"2 ABC"），「1 分词」和「重输」是九宫格
// 自己加的两个动作键，不占组号（组号仍然只有 8 个：abc..wxyz）。
static void evkNineGridDraw(int w, int hLet) {
    int pw, cx, cw, rx, rw;
    evkNineCols(w, &pw, &cx, &cw, &rx, &rw);
    const int gy = evkRowY(evkLetter0());
    const int gh = 3 * hLet;

    // 左列：没有组合时是标点键（键面写全角，发出去的仍是半角 ASCII——中文态由 IME 的
    // handleFullwidthPunct 自己转成全角，和 26 键第三行那两个标点键同一路子）；有组合
    // 时整列换成**分音节选择列**，和 T9 面板左列同一个东西。
    if (evkNineSylOn()) {
        evkNineSylColDraw(pw, gy, gh);
    } else {
        static const char *const PUNCT[4] = {"，", "。", "？", "！"};
        for (int i = 0; i < 4; i++) {
            int py, ph;
            evkNinePunctCell(gy, gh, i, &py, &ph);
            evkKey(0, py, pw, ph, PUNCT[i], false);
        }
    }

    // 中间九宫格：(0,0) 是「1 分词」，其余八格按行铺 2~9，组号 = 格序 - 1
    // （abc 是组 0 → 在格 1，正好对应手机上的"2 abc"）。
    char lb[12];
    for (int r = 0; r < 3; r++) {
        const int y = gy + r * hLet;
        for (int c = 0; c < 3; c++) {
            const int idx = r * 3 + c;
            const int x = cx + c * cw;
            evkKey(x, y, cw, hLet, evkNineKeyLabel(idx == 0 ? -1 : idx - 1, cw, lb, sizeof(lb)), false);
        }
    }

    // 右列：⌫ / 重输 / 0，与三个字母行一一对齐。
    evkBackspaceKey(rx, gy, rw, hLet, false);
    evkKey(rx, gy + hLet, rw, hLet, "重输", false);
    evkKey(rx, gy + 2 * hLet, rw, hLet, "0", false);
}

// ── 符号面板 / 数字面板的绘制 ────────────────────────────────────────────

// 符号面板：10 列 × 3 行，格子与字母页完全一样（行高、居中方式、满宽），只是内容
// 换成标点（见 EVK_SYM_PANEL）。键数固定 10，所以直接按第一行的基准画，三行左右对齐。
static void evkSymPanelDraw(int w, int hLet) {
    const int keyW = w / 10;
    const int x0 = evkRow3X(w, keyW);   // (w - 10*keyW)/2，和 qwerty 首行同一基准
    for (int r = 0; r < 3; r++) {
        const char *row = EVK_SYM_PANEL[r];
        const int ly = evkRowY(evkLetter0() + r);
        for (int k = 0; row[k]; k++) {
            char key[2] = {row[k], 0};
            evkKey(x0 + k * keyW, ly, keyW, hLet, key, false);
        }
    }
}

// 数字面板：左列(# * - +) | 中间三列(1..9，末行 0 横跨三列) | 右列(⌫ . @ /)。
// 四行铺满整条字母行带（几何见 evkNumRowY/evkNumRowH）。0 横跨三列是为了好按——
// 手机上那张盘的 0 也是宽键，而且它旁边就是最常用的退格/小数点，窄了容易点错。
// 退格走图标键帽（字体子集里没有 ⌫ 的码点，见 evkBackspaceKey），标签留空。
static void evkNumPanelDraw(int w) {
    const int side = evkNumSideW(w);
    const int colW = (w - 2 * side) / 3;
    for (int r = 0; r < 4; r++) {
        const int ry = evkNumRowY(r), rh = evkNumRowH(r);
        char lk[2] = {EVK_NUM_LEFT[r], 0};
        evkKey(0, ry, side, rh, lk, false);                       // 左列
        if (r < 3) {                                              // 中间三列：1..9
            for (int c = 0; c < 3; c++) {
                char dk[2] = {static_cast<char>('1' + r * 3 + c), 0};
                evkKey(side + c * colW, ry, colW, rh, dk, false);
            }
        } else {
            evkKey(side, ry, colW * 3, rh, "0", false);           // 末行：0 横跨三列
        }
        if (r == 0) {
            evkBackspaceKey(w - side, ry, side, rh, false);       // 右列第一格：退格
        } else {
            char rk[2] = {EVK_NUM_RIGHT[r], 0};
            evkKey(w - side, ry, side, rh, rk, false);
        }
    }
}

// 底部功能行最左那两个键的键面文字（which: 0=① 1=②）。三个面板绕一圈：
//   字母页 ①符 ②123   符号面板 ①ABC ②123   数字面板 ①符 ②ABC
// 两个键各自只说"按下去会去哪"，不做"当前在哪"的指示——当前在哪看键区一眼就知道，
// 键帽上再标一遍反而要多认一个字。①②的作用也一样（都是换面板），只是目标不同。
static const char *evkPanelKeyLabel(int which) {
    if (evkNumPage()) return which == 0 ? "符" : "ABC";
    if (evkSymPage()) return which == 0 ? "ABC" : "123";
    return which == 0 ? "符" : "123";
}

void editorVkDraw() {
    // 键盘面板是界面框架：在编辑器正文作用域里也会被调到，钉回界面字号。
    FontScope ui(FontRenderer::uiPxHeight());
    UI_FONT_GUARD();
    if (!s_visible) return;
    int w = SCREEN_W;
    int top = editorVkTop();
    int bottom = STATUS_BAR_Y;
    if (top < FONT_H || bottom - top < 16) return;   // 屏太小，放弃绘制

    // 面板白底 + 顶边/编码-候选/候选-键区三条分隔线
    u8g2_SetDrawColor(g_u8g2, 1);
    u8g2_DrawBox(g_u8g2, 0, top, w, bottom - top);
    u8g2_SetDrawColor(g_u8g2, 0);
    u8g2_DrawHLine(g_u8g2, 0, top, w);
    u8g2_DrawHLine(g_u8g2, 0, top + evkCandRowH(0), w);
    u8g2_DrawHLine(g_u8g2, 0, top + evkCandH(), w);

    // ── 候选区上行：编码(拼音串)。右端挂页号，>1 页时才画 ──
    // 和 ui_helpers 的 drawIMEUI 同一条编码行，必须用同一套字体口径：**g_font**
    // （CONTENT 面 + 拉丁钉回内置等宽）。用 g_content_font 的话，用户选了非内置字体时
    // 比例拉丁会被居中塞进恒定的半格里 —— 字母间距忽疏忽密。量宽/画字两处都换成 g_font，
    // 免得页号量得出来、画出来对不上（静态格子共用，行高基线不受影响）。
    IME &ime = IME::getInstance();
    std::string code = ime.displayCode();
    if (!code.empty()) g_font.drawText(6, evkCandTextBaseline(0), code.c_str(), false);
    int pages = ime.totalPages();
    if (pages > 1) {
        // "< 1/3 >"：左右两个尖括号就是"往左右划"的提示，比再写一行说明省地方。
        std::string pg = "< " + std::to_string(ime.currentPage()) + "/" +
                         std::to_string(pages) + " >";
        int tw = g_font.textWidth(pg.c_str());
        g_font.drawText(w - 6 - tw, evkCandTextBaseline(0), pg.c_str(), false);
    }

    // ── 候选区下行：候选(高亮项反白)，整行宽度都归它 ──
    evkComputeCandLayout();
    int candTextY = evkCandTextBaseline(1);
    int hl = ime.highlightIdx();
    int hlY = evkCandHlY(1);
    int hlH = evkCandHlH();
    for (size_t i = 0; i < s_candLabel.size(); i++) {
        bool sel = (static_cast<int>(i) == hl);
        if (sel) {
            u8g2_SetDrawColor(g_u8g2, 0);
            u8g2_DrawBox(g_u8g2, s_candX[i] - 2, hlY, s_candW[i] + 4, hlH);
            u8g2_SetDrawColor(g_u8g2, 1);
        }
        evkCandDrawText(s_candX[i], candTextY, s_candLabel[i].c_str(), sel);
    }

    // ── T9 候选面板：点开"编码"行后，整块键区换成 读音列 + 候选宫格 ──
    if (evkT9Active()) {
        evkT9Draw(w);
        if (s_pressedValid) evkDrawPressedInv(s_pressed);
        u8g2_SetDrawColor(g_u8g2, 1);
        return;
    }

    // ── 键区 ──
    int keyW = w / 10;
    const int hCtrl = evkRowH(0);
    const int hLet = evkRowH(evkLetter0());
    const int hFunc = evkRowH(evkFuncRow());

    // 固定额外行：[布局] [Ctl] [◀ ▲ ▼ ▶] [Shift] [⌫]，宽度见 evkExtraRowRects。
    // 九宫格下没有末位的 ⌫（它挪进了九宫格右列），版式见 evkNineExtraRowRects。
    const bool nine = evkNineKey();
    // 符号面板 / 数字面板：整条字母行带换成另一张键表，见 evkSymPanelDraw/evkNumPanelDraw。
    const bool symPanel = evkSymPage();
    const bool numPanel = evkNumPage();
    int ry = evkRowY(0);
    int ex[8], ew[8];
    if (nine) {
        int nx[7], nw[7];
        evkNineExtraRowRects(w, keyW, nx, nw);
        for (int i = 0; i < 7; i++) { ex[i] = nx[i]; ew[i] = nw[i]; }
        ex[7] = w; ew[7] = 0;   // 右端没有第八段
    } else {
        evkExtraRowRects(w, keyW, ex, ew);
    }
    evkKey(ex[0], ry, ew[0], hCtrl, evkLayoutLabelEff(), evkAmbig());
    evkKey(ex[1], ry, ew[1], hCtrl, "Ctl", s_ctrl);
    for (int d = 0; d < 4; d++) evkTriangleKey(ex[2 + d], ry, ew[2 + d], hCtrl, d);
    evkShiftKey(ex[6], ry, ew[6], hCtrl, s_shift);
    if (!nine) evkBackspaceKey(ex[7], ry, ew[7], hCtrl, false);

    const char *const *rows = evkRows();
    const bool ambig = evkAmbig();
    if (nine) {
        evkNineGridDraw(w, hLet);
    } else if (numPanel) {
        evkNumPanelDraw(w);
    } else if (symPanel) {
        evkSymPanelDraw(w, hLet);
    } else if (ambig) {
        // 一键多字母：每行的组等宽平分整个屏宽（见 evkAmbigRowRects）。键面写组号对应的
        // 标签（9 键是 "2 abc" 这种，见 IME::ambigGroupLabel）。
        const int kMaxAmbig = 8;   // 18 键第一行 7 组，留一格余量
        int xs[kMaxAmbig], ws[kMaxAmbig];
        for (int r = 0; r < IME::ambigRowCount(); r++) {
            evkAmbigRowRects(r, w, keyW, xs, ws);
            int ly = evkRowY(evkLetter0() + r);
            for (int k = 0; k < IME::ambigRowKeys(r) && k < kMaxAmbig; k++) {
                evkKey(xs[k], ly, ws[k], hLet, IME::ambigGroupLabel(IME::ambigRowGroup(r, k)), false);
            }
        }
    }
    for (int r = 0; r < (ambig || nine || numPanel || symPanel ? 0 : 2); r++) {
        const char *row = rows[r];
        int n = static_cast<int>(strlen(row));
        int kx = (w - n * keyW) / 2;   // 逐行居中(qwerty 10 键 / asdf 9 键)
        int ly = evkRowY(evkLetter0() + r);
        for (int k = 0; k < n; k++) {
            char key[2] = {row[k], 0};
            evkKey(kx + k * keyW, ly, keyW, hLet, key, false);
        }
    }

    // 第三行
    int r3y = evkRowY(evkLetter0() + 2);
    if (ambig || nine || numPanel || symPanel) {
        // 一键多字母 / 九宫格 / 两张面板的第三行已在上面一起画完
    } else {
        // 字母页：zxcvbnm | ，。 共 9 格，和 asdfghjkl 一样逐行居中（Shift 已移到
        // 额外行、退格左边）。标点键中英态换标签(全角/半角)。
        bool cn = evkCnPunct();
        int l3x = (w - 9 * keyW) / 2;
        for (int k = 0; EVK_LETTER_ROW3_MID[k]; k++) {
            char key[2] = {EVK_LETTER_ROW3_MID[k], 0};
            evkKey(l3x + k * keyW, r3y, keyW, hLet, key, false);
        }
        evkKey(l3x + 7 * keyW, r3y, keyW, hLet, cn ? "，" : ",", false);
        evkKey(l3x + 8 * keyW, r3y, keyW, hLet, cn ? "。" : ".", false);
    }

    // 功能行：①面板切换 | ②面板切换 | 空格(图标) | 中/英 | 回车(图标,两格)。
    // 退格已上移到额外行。①②的标签随当前面板变（见 evkPanelKeyLabel），中/英紧挨着
    // 回车。回车仍是两格——图标宽度按键宽走，两格才够画出一个不塌的 ↵。
    int by = evkRowY(evkFuncRow());
    int fx[5], fw[5];
    evkFuncRowRects(w, keyW, fx, fw);
    evkKey(fx[0], by, fw[0], hFunc, evkPanelKeyLabel(0), false);
    evkKey(fx[1], by, fw[1], hFunc, evkPanelKeyLabel(1), false);
    evkSpaceKey(fx[2], by, fw[2], hFunc, false);
    bool en = ime.english();
    const char *lang = !ime.active() ? "中" : (en ? "英" : "拼");
    evkKey(fx[3], by, fw[3], hFunc, lang, en);
    evkReturnKey(fx[4], by, fw[4], hFunc, false);

    // 刚按下的键：最后补一遍反色，压在常态之上（不额外推屏，随本次重绘上屏）。
    if (s_pressedValid) evkDrawPressedInv(s_pressed);

    u8g2_SetDrawColor(g_u8g2, 1);
}

void editorVkDrawIcon() {
    FontScope ui(FontRenderer::uiPxHeight());   // 状态栏右端的键盘开关图标：界面框架
    UI_FONT_GUARD();
    int slot = editorVkIconSlotW();
    if (slot <= 0) return;
    int h = STATUS_BAR_H - 2;
    int y = STATUS_BAR_Y + 1;
    if (h < 8) return;
    // 反白表示键盘已展开
    bool on = s_visible;
    u8g2_SetDrawColor(g_u8g2, on ? 0 : 1);
    if (on) u8g2_DrawBox(g_u8g2, SCREEN_W - slot, y, slot, h);
    u8g2_SetDrawColor(g_u8g2, on ? 1 : 0);
    // 键盘示意：外框 + 三个键帽
    int bw = slot - 20;
    int bh = h / 3;
    if (bw < 10) bw = slot / 2;
    if (bh < 6) bh = 6;
    int bx = SCREEN_W - slot + (slot - bw) / 2;
    int byy = y + (h - bh) / 2;
    u8g2_DrawFrame(g_u8g2, bx, byy, bw, bh);
    int kw = (bw - 8) / 3;
    for (int i = 0; i < 3; i++) {
        u8g2_DrawBox(g_u8g2, bx + 2 + i * (kw + 1), byy + 2, kw, 2);
    }
    u8g2_SetDrawColor(g_u8g2, 1);
}

// 命中即回填几何：按下反馈照同一份矩形反色重画，不必再猜一遍。
static void evkSetHit(EditorVkHit *hit, int x, int y, int w, int h, int shape, int dir, bool filled,
                      const char *text) {
    if (!hit) return;
    hit->x = x; hit->y = y; hit->w = w; hit->h = h;
    hit->shape = shape;
    hit->dir = dir;
    hit->filled = filled;
    hit->text[0] = 0;
    if (text) {
        size_t n = strlen(text);
        if (n > sizeof(hit->text) - 1) n = sizeof(hit->text) - 1;
        memcpy(hit->text, text, n);
        hit->text[n] = 0;
    }
}

// 文字键帽的命中：椭圆判定 + 回填(单字符用 char 快路径，避免临时串)。
static bool evkHitKeyChar(int px, int py, int kx, int ky, int kw, int kh, char c, EditorVkHit *hit) {
    if (!evkInEllipse(px, py, kx, ky, kw, kh)) return false;
    char buf[2] = {c, 0};
    evkSetHit(hit, kx, ky, kw, kh, 0, 0, false, buf);
    return true;
}

// 九宫格键区的命中（只给 9 键布局用，几何与 evkNineGridDraw 共用 evkNineCols）。
// 返回 pjournal 键码或 EVK_NONE，命中时回填 hit 几何给按下反馈用。
static int evkNineGridHit(int x, int y, int w, int hLet, EditorVkHit *hit) {
    int pw, cx, cw, rx, rw;
    evkNineCols(w, &pw, &cx, &cw, &rx, &rw);
    const int gy = evkRowY(evkLetter0());
    const int gh = 3 * hLet;
    if (y < gy || y >= gy + gh) return EVK_NONE;

    // 左列：组合中 = 分音节选择列，点一行就把这个音节定下来（和 T9 面板左列同一件事，
    // 同一个 selectAmbigCode）；否则 = 标点键。
    if (x < pw) {
        IME &ime = IME::getInstance();
        if (evkNineSylOn()) {
            const std::vector<std::string> &codes = ime.ambigSyllables();
            int n, rh, vis, first, last;
            evkNineSylWindow(gh, &n, &rh, &vis, &first, &last);
            for (int i = first; i < last; i++) {
                const int cy = gy + (i - first) * rh;
                if (!evkInEllipse(x, y, 0, cy, pw - 2, rh)) continue;
                // 先回带文字再选：selectAmbigCode 会把这张表整个重算，之后就取不到了。
                // （反色按旧文字画没关系，下一帧整列都会重画成新内容。）
                evkSetHit(hit, 0, cy, pw, rh, 0, 0, false, codes[i].c_str());
                ime.selectAmbigCode(i);
                return EVK_SEP;
            }
            return EVK_NONE;
        }
        // 标点：发半角码，中文态由 IME 转全角（和 26 键那两个标点键一致）。
        static const char *const PUNCT_LAB[4] = {"，", "。", "？", "！"};
        static const char PUNCT_KEY[4] = {',', '.', '?', '!'};
        for (int i = 0; i < 4; i++) {
            int py, ph;
            evkNinePunctCell(gy, gh, i, &py, &ph);
            if (!evkInEllipse(x, y, 0, py, pw, ph)) continue;
            evkSetHit(hit, 0, py, pw, ph, 0, 0, false, PUNCT_LAB[i]);
            return evkFinishKey(static_cast<unsigned char>(PUNCT_KEY[i]));
        }
        return EVK_NONE;
    }

    // 右列功能：⌫ / 重输 / 0，与三个字母行一一对齐。
    if (x >= rx) {
        const int ry[3] = {gy, gy + hLet, gy + 2 * hLet};
        if (evkInEllipse(x, y, rx, ry[0], rw, hLet)) {
            evkSetHit(hit, rx, ry[0], rw, hLet, 4, 0, false, nullptr);
            return evkFinishKey('\b');
        }
        if (evkInEllipse(x, y, rx, ry[1], rw, hLet)) {
            // 「重输」= 手机上那个清空当前输入键。这里只清**拼音组合**，绝不动正文：
            // 用户的正文是攒出来的，一次误触就抹掉是不可接受的代价。没在组合时无动作。
            evkSetHit(hit, rx, ry[1], rw, hLet, 0, 0, false, "重输");
            IME::getInstance().cancelComposition();
            return EVK_CLEAR;
        }
        if (evkInEllipse(x, y, rx, ry[2], rw, hLet)) {
            evkSetHit(hit, rx, ry[2], rw, hLet, 0, 0, false, "0");
            // 组合中按 0 什么都不做：IME 把"非拼音键"一律当作"上屏首选词然后收尾"，
            // 于是用户按的是 0、上屏的却是一个词。与其造成这种意外，不如在组合期间
            // 让这个键静默——想要 0 就先按回车/空格把拼音落下去，那时它就是一个 0。
            // 返回 EVK_NOOP 而不是 EVK_NONE：键确实被按到了，反色反馈要照给。
            if (IME::getInstance().composing()) return EVK_NOOP;
            return evkFinishKey('0');
        }
        return EVK_NONE;
    }

    // 中间九宫格。evkNineKey() 保证这里是"输入法开着且中文态"，所以不用像 14/18/9 键
    // 那条老路那样再判一次英文态退化。
    for (int r = 0; r < 3; r++) {
        const int ky = gy + r * hLet;
        if (y < ky || y >= ky + hLet) continue;
        for (int c = 0; c < 3; c++) {
            const int kx = cx + c * cw;
            if (!evkInEllipse(x, y, kx, ky, cw, hLet)) continue;
            const int idx = r * 3 + c;
            if (idx == 0) {
                // 「1 分词」：确定下一个音节（命中测试内已生效），候选跟着重算。
                char lb[12];
                evkSetHit(hit, kx, ky, cw, hLet, 0, 0, false,
                          evkNineKeyLabel(-1, cw, lb, sizeof(lb)));
                IME::getInstance().ambigCommitSyllable();
                return EVK_SEP;
            }
            const int g = idx - 1;
            char lb[12];
            evkSetHit(hit, kx, ky, cw, hLet, 0, 0, false,
                      evkNineKeyLabel(g, cw, lb, sizeof(lb)));
            return IME::KEY_AMBIG_BASE + g;
        }
        return EVK_NONE;
    }
    return EVK_NONE;
}

// ── T9 候选面板 ──────────────────────────────────────────────────────────
// 点候选区上行的「编码」行切换：候选区之下的整块键区换成"左列读音 + 右面候选宫格"，
// 和安卓九宫格输入法点开候选后的那个面板一个意思。好处有两个：候选从"一条线"变成
// 一片大格子，指尖不必再挤在十几个像素的间距里挑；有多个展开读音时（歧义布局的
// 编码）左列能先挑读音，再在右边挑字。
//
// 只在输入法正在组合(composing)时有意义：组合一结束（上屏完/取消/退格到空）自动
// 收起，不用再点一次；再点编码行也能直接收起。面板里点候选 = 点候选条上的同一格
// （返回 '1' + 页内序号），输入逻辑一行不改。
static bool evkT9Active() {
    if (!s_t9) return false;
    if (!IME::getInstance().composing()) { evkT9Set(false); return false; }
    return true;
}

// 面板区域 = 候选区之下的整块键区（与原来的键区完全重合，只是换了内容）。
static int evkT9Top() { return editorVkTop() + evkCandH() + EVK_PAD; }
static int evkT9Bot() { return STATUS_BAR_Y; }

// 左列(分音节选择列)宽度；一个可挑的都没有才返回 0（宫格占满整宽）。
// 只有一个选项也照画：它是"要按一下才生效"的选择，不是状态显示——少了这一列
// 用户就没法把它定下来（这一步的候选也就收窄不了）。按最长音节实测宽度加留白，
// 夹在 [1.6 格, 3 格] —— 竖屏也要放得下四个字母的拼音。
static int evkT9LeftW(int w, int keyW, int n) {
    if (n < 1) return 0;
    const std::vector<std::string> &codes = IME::getInstance().ambigSyllables();
    int tw = 0;
    for (int i = 0; i < n && i < (int)codes.size(); i++) {
        int t = g_vk_font.textWidth(codes[i].c_str());
        if (t > tw) tw = t;
    }
    int lw = tw + 20;
    const int lo = keyW * 8 / 5, hi = keyW * 3;
    if (lw < lo) lw = lo;
    if (lw > hi) lw = hi;
    if (lw > w / 3) lw = w / 3;
    return lw;
}

// 宫格列数：3 列 × 3 行 = 9 格，正好是 IME::GRID_PAGE_SIZE（宫格展开时 IME 强制每页 9 个，
// 外来 setPageSize 改不动它）。行数由实际候选数现算，最后一页不满时不会留空格线。
#define EVK_T9_COLS 3

// 左列读音表的行高：按行数等分，但**有下限**——歧义编码最多能展开出 12 个读音
// （lookupAmbiguous 每轮 12 个），等分下来一行只有 33px，两个拼音会叠在一起。
// 行高触到下限就装不满整列，剩下的靠 s_t9CodeTop 滚动看（面板支持上下滑的原因）。
// 上限仍是 2 格：读音少的时候不必每行空荡荡一大条。
// 滚动条宽度：装不下时才占位置。
#define EVK_T9_BAR_W 5
static int evkT9CodeRowH(int h, int n) {
    if (n <= 1) return h;
    int rh = h / n;
    const int cap = EVK_ROW_H * 2;
    if (rh > cap) rh = cap;
    const int lo = FONT_H + 10;   // 下限：一行至少要放得下一个字号 + 行距
    if (rh < lo) rh = lo;
    if (rh > h) rh = h;
    return rh;
}
static int evkT9CodeVis(int h, int rh) {
    int v = (rh > 0) ? h / rh : 1;
    return v < 1 ? 1 : v;
}
// 滚动位置夹进合法范围（并写回状态）：窗口不能翻过头，也不能为负。
static int evkT9CodeFirst(int n, int vis) {
    int first = s_t9CodeTop;
    if (first > n - vis) first = n - vis;
    if (first < 0) first = 0;
    s_t9CodeTop = first;
    return first;
}

// 九宫格左列音节表的窗口：和 T9 面板左列共用 evkT9CodeRowH/Vis/First（含滚动位置
// s_t9CodeTop 本身——两边不会同时出现，共用一个正好）。窗口放进 *first..*last，
// 返回这一轮算出来的行高（<0 表示这一列现在不该画音节）。
static int evkNineSylWindow(int gh, int *n, int *rh, int *vis, int *first, int *last) {
    const int cnt = (int)IME::getInstance().ambigSyllables().size();
    const int r = evkT9CodeRowH(gh, cnt);
    const int v = evkT9CodeVis(gh, r);
    const int f = evkT9CodeFirst(cnt, v);
    *n = cnt;
    *rh = r;
    *vis = v;
    *first = f;
    *last = cnt < f + v ? cnt : f + v;
    return r;
}

// 九宫格左列在组合中的样子：一列"剩下的键还能读成哪几个音节"，点一个就把它定下来
// ——和 T9 面板左列是同一件事（同一个 selectAmbigCode）。滚动条照画，否则用户不知道
// 下面还有（上下滑滚它，见 editorVkSwipeScroll）。
static void evkNineSylColDraw(int lw, int gy, int gh) {
    const std::vector<std::string> &codes = IME::getInstance().ambigSyllables();
    int n, rh, vis, first, last;
    evkNineSylWindow(gh, &n, &rh, &vis, &first, &last);
    const int barW = (n > vis && lw > 40) ? EVK_T9_BAR_W : 0;
    const std::string cur = IME::getInstance().displayCode();
    u8g2_SetDrawColor(g_u8g2, 0);
    u8g2_DrawVLine(g_u8g2, lw - 2, gy, gh);
    for (int i = first; i < last; i++) {
        const int y = gy + (i - first) * rh;
        const bool sel = (codes[i] == cur);
        // 反白块自己填整行（字数少时 drawText 的自填只盖住字身），字再反色盖上去。
        if (sel) u8g2_DrawBox(g_u8g2, 1, y + 1, lw - 4 - barW, rh - 2);
        const int tw = g_vk_font.textWidth(codes[i].c_str());
        int tx = (lw - 5 - barW - tw) / 2;   // 居中区再让开滚动条那几像素
        if (tx < 4) tx = 4;
        g_content_font.drawText(tx, y + (rh + g_vk_font.ascent() - g_vk_font.descent()) / 2,
                                codes[i].c_str(), sel);
    }
    if (barW > 0) {   // 滚动条：槽 + 按窗口比例算的滑块
        const int bx = lw - 3 - barW;
        u8g2_DrawFrame(g_u8g2, bx, gy, barW, gh);
        int th = gh * vis / n;
        if (th < 12) th = 12;
        const int space = n - vis;
        const int ty = gy + (space > 0 ? (gh - th) * first / space : 0);
        u8g2_DrawBox(g_u8g2, bx + 1, ty + 1, barW - 2, th - 2);
    }
    u8g2_SetDrawColor(g_u8g2, 1);
}

// 面板绘制(键区位置)。与 evkT9Hit 共用同一份几何，一处算宽、一处算格。
static void evkT9Draw(int w) {
    IME &ime = IME::getInstance();
    const std::vector<std::string> &cands = ime.candidates();
    const std::vector<std::string> &codes = ime.ambigSyllables();
    const int top = evkT9Top(), bot = evkT9Bot();
    const int h = bot - top;
    if (h < 40) return;
    const int keyW = w / 10;
    const int lw = evkT9LeftW(w, keyW, (int)codes.size());

    // 左列：**分音节选择**——剩下的键还能拼出哪些音节，点一个就把这个音节定下来，
    // 右面的候选随即只按这个前缀取（所以这一列是"选项"不是"当前读音"，没有反白项）。
    // 装不下时只画窗口里的几行，右边一条滚动条告诉你上下还有。
    if (lw > 0) {
        const std::string cur = ime.displayCode();
        const int n = (int)codes.size();
        const int rh = evkT9CodeRowH(h, n);
        const int vis = evkT9CodeVis(h, rh);
        const int first = evkT9CodeFirst(n, vis);
        const int last = n < first + vis ? n : first + vis;
        const int barW = (n > vis && lw > 40) ? EVK_T9_BAR_W : 0;
        u8g2_SetDrawColor(g_u8g2, 0);
        u8g2_DrawVLine(g_u8g2, lw - 2, top, h);
        for (int i = first; i < last; i++) {
            const int y = top + (i - first) * rh;
            const bool sel = (codes[i] == cur);
            // 反白块自己填整行(字数少时 drawText 的自填只盖住字身)，字再反色盖上去。
            if (sel) u8g2_DrawBox(g_u8g2, 1, y + 1, lw - 4 - barW, rh - 2);
            int tw = g_vk_font.textWidth(codes[i].c_str());
            int tx = (lw - 5 - barW - tw) / 2;   // 居中区再让开滚动条那几像素
            if (tx < 4) tx = 4;
            g_content_font.drawText(tx, y + (rh + g_vk_font.ascent() - g_vk_font.descent()) / 2,
                                    codes[i].c_str(), sel);
        }
        if (barW > 0) {   // 滚动条：槽 + 按窗口比例算的滑块
            const int bx = lw - 3 - barW;   // 紧贴分隔线左侧，反白块不会压到槽上
            u8g2_DrawFrame(g_u8g2, bx, top, barW, h);
            int th = h * vis / n;
            if (th < 12) th = 12;
            const int space = n - vis;
            int ty = top + (space > 0 ? (h - th) * first / space : 0);
            u8g2_DrawBox(g_u8g2, bx + 1, ty + 1, barW - 2, th - 2);
        }
        u8g2_SetDrawColor(g_u8g2, 1);
    }

    // 右面：当前一页的候选铺成宫格。点哪一格 = 按哪个数字键。
    const int n = (int)cands.size();
    if (n <= 0) return;
    const int gx = lw, gw = w - lw;
    const int rows = (n + EVK_T9_COLS - 1) / EVK_T9_COLS;
    const int cw = gw / EVK_T9_COLS, ch = h / rows;
    if (cw < 20 || ch < 20) return;
    u8g2_SetDrawColor(g_u8g2, 0);
    for (int j = 0; j < n; j++)
        u8g2_DrawFrame(g_u8g2, gx + (j % EVK_T9_COLS) * cw, top + (j / EVK_T9_COLS) * ch, cw, ch);
    u8g2_SetDrawColor(g_u8g2, 1);
    for (int j = 0; j < n; j++) {
        const int cx = gx + (j % EVK_T9_COLS) * cw, cy = top + (j / EVK_T9_COLS) * ch;
        std::string lab = editorVkTruncateToWidth(cands[j], cw - 12);
        int tw = g_vk_font.textWidth(lab.c_str());
        g_content_font.drawText(cx + (cw - tw) / 2, cy + (ch + g_vk_font.ascent() - g_vk_font.descent()) / 2,
                                lab.c_str(), false);
    }
    // 页码不在这里标：宫格四角就是格线，压一块白底会把角上那格的候选字擦掉一半；
    // 候选条右端本来就有页码，翻页时宫格里的字整批换掉，看得见。
}

// 面板命中：左列挑音节(整块面板会重画 → 返回 EVK_T9)，右面点候选(返回 '1'+页内序号)。
static int evkT9Hit(int x, int y, int w, EditorVkHit *hit) {
    IME &ime = IME::getInstance();
    const std::vector<std::string> &cands = ime.candidates();
    const std::vector<std::string> &codes = ime.ambigSyllables();
    const int top = evkT9Top(), bot = evkT9Bot();
    const int h = bot - top;
    if (h < 40 || y < top || y >= bot) return EVK_NONE;
    const int keyW = w / 10;
    const int lw = evkT9LeftW(w, keyW, (int)codes.size());

    if (lw > 0) {
        const int n = (int)codes.size();
        const int rh = evkT9CodeRowH(h, n);
        const int vis = evkT9CodeVis(h, rh);
        const int first = evkT9CodeFirst(n, vis);
        const int last = n < first + vis ? n : first + vis;
        for (int i = first; i < last; i++) {
            if (!evkInEllipse(x, y, 0, top + (i - first) * rh, lw - 2, rh)) continue;
            evkSetHit(hit, 0, 0, 0, 0, 0, 0, false, nullptr);   // 不补反色，面板本身会重画
            return ime.selectAmbigCode(i) ? EVK_T9 : EVK_NONE;
        }
    }

    const int n = (int)cands.size();
    if (n <= 0) return EVK_NONE;
    const int gx = lw, gw = w - lw;
    const int rows = (n + EVK_T9_COLS - 1) / EVK_T9_COLS;
    const int cw = gw / EVK_T9_COLS, ch = h / rows;
    if (cw < 20 || ch < 20) return EVK_NONE;
    for (int j = 0; j < n; j++) {
        const int cx = gx + (j % EVK_T9_COLS) * cw, cy = top + (j / EVK_T9_COLS) * ch;
        if (!evkInEllipse(x, y, cx, cy, cw, ch)) continue;
        // 不补按下反色：这一下会立刻选词上屏、面板跟着重画（组合结束还会整块收起），
        // 留着的反色块会画到"下一个界面"上——面板收起后那些坐标已经是字母键了。
        evkSetHit(hit, 0, 0, 0, 0, 0, 0, false, nullptr);
        return '1' + j;                       // 与候选条上的同一格等价
    }
    return EVK_NONE;
}

int editorVkHitTest(int x, int y, EditorVkHit *hit) {
    // 键位命中必须与 editorVkDraw 画出来的格子逐像素对齐 —— 那是界面字号下的几何，
    // 所以这里也要钉回界面字号（本函数在编辑器正文作用域里被调到）。
    FontScope ui(FontRenderer::uiPxHeight());
    if (!s_visible) return EVK_NONE;
    int w = SCREEN_W;
    int top = editorVkTop();
    // 与 editorVkDraw 同一道门：屏太小/候选字号太大导致面板放不下时，编辑器根本不画
    // 键盘。这时不能再把这片区域的点按翻译成按键 —— 否则看不见的空格/回车照样往正文
    // 里塞字（画的和认的两套几何必须同进退）。
    if (top < FONT_H || STATUS_BAR_Y - top < 16) return EVK_NONE;
    if (y < top || y >= STATUS_BAR_Y) return EVK_NONE;

    // 候选区：上行编码没有可点的东西(整行吞掉，别漏给下面的键区)，下行是候选块。
    // 候选块本就是小矩形，仍走椭圆(块很窄，角上误触代价是选错字)。
    if (y < top + evkCandH()) {
        if (y < top + evkCandRowH(0)) {
            // 编码行：点它开/关 T9 候选面板（候选多了以后，一行候选条不够点）。
            // 没在组合时这一行本来就是空的，点了也不响应。
            IME &ime = IME::getInstance();
            if (!ime.composing()) return EVK_NONE;
            evkT9Set(!s_t9);
            evkSetHit(hit, 0, 0, 0, 0, 0, 0, false, nullptr);   // 开关本身就是反馈，不补反色
            return EVK_T9;
        }
        evkComputeCandLayout();
        IME &ime = IME::getInstance();
        int ps = ime.pageSize();
        int hlY = evkCandHlY(1);
        int hlH = evkCandHlH();
        for (size_t i = 0; i < s_candX.size(); i++) {
            int cx = s_candX[i] - 2, cw = s_candW[i] + 4;
            if (evkInEllipse(x, y, cx, hlY, cw, hlH)) {
                evkSetHit(hit, cx, hlY, cw, hlH, 3, 0,
                          static_cast<int>(i) == ime.highlightIdx(), s_candLabel[i].c_str());
                return '1' + static_cast<int>(i % ps);   // 与物理键盘按数字键等价
            }
        }
        return EVK_NONE;
    }

    // T9 面板展开时，候选区之下的整块键区归面板（读音列 + 候选宫格）。
    if (evkT9Active()) return evkT9Hit(x, y, w, hit);

    int keyW = w / 10;
    const bool nine = evkNineKey();

    // 额外行：[布局] [Ctl] [◀ ▲ ▼ ▶] [Shift] [⌫]，横竖屏一致。
    // 九宫格下没有末位的 ⌫（它在九宫格右列），几何见 evkNineExtraRowRects。
    {
        int ry = evkRowY(0), rh = evkRowH(0);
        if (y >= ry && y < ry + rh) {
            int ex[8], ew[8];
            if (nine) {
                int nx[7], nw[7];
                evkNineExtraRowRects(w, keyW, nx, nw);
                for (int i = 0; i < 7; i++) { ex[i] = nx[i]; ew[i] = nw[i]; }
                ex[7] = w; ew[7] = 0;
            } else {
                evkExtraRowRects(w, keyW, ex, ew);
            }
            // 布局键：先按"按下前"的常态回填，再翻转（闪的就是这一下）。
            // 英文态下布局被强制成 26 键(见 evkAmbig)，这时按键只给一下反色反馈、
            // 不改 s_layout：改了也看不见，留着等切回中文时生效反而更让人糊涂。
            if (evkInEllipse(x, y, ex[0], ry, ew[0], rh)) {
                const bool cn = evkCnPunct();
                evkSetHit(hit, ex[0], ry, ew[0], rh, 0, 0, evkAmbig(), evkLayoutLabelEff());
                // 26→14→18→9→26。改完写设置：下次开机还是这个布局（9 键用户不该每次
                // 开机再点三下）。写只在这一次点按上发生，绘制路径不碰。
                if (cn) editorVkSetLayout(evkLayoutKey((s_layout + 1) % IME::ambigLayoutCount()));
                return EVK_LAYOUT;
            }
            if (evkInEllipse(x, y, ex[1], ry, ew[1], rh)) {
                evkSetHit(hit, ex[1], ry, ew[1], rh, 0, 0, s_ctrl, "Ctl");
                s_ctrl = !s_ctrl;
                return EVK_CTRL;
            }
            static const int DIRKEY[4] = {KEY_LEFT, KEY_UP, KEY_DOWN, KEY_RIGHT};
            for (int d = 0; d < 4; d++) {
                if (evkInEllipse(x, y, ex[2 + d], ry, ew[2 + d], rh)) {
                    evkSetHit(hit, ex[2 + d], ry, ew[2 + d], rh, 1, d, false, nullptr);
                    return evkFinishKey(DIRKEY[d]);
                }
            }
            if (evkInEllipse(x, y, ex[6], ry, ew[6], rh)) {
                evkSetHit(hit, ex[6], ry, ew[6], rh, 2, 0, s_shift, nullptr);
                s_shift = !s_shift;
                return EVK_SHIFT;
            }
            if (!nine && evkInEllipse(x, y, ex[7], ry, ew[7], rh)) {
                evkSetHit(hit, ex[7], ry, ew[7], rh, 4, 0, false, nullptr);
                return evkFinishKey('\b');
            }
            return EVK_NONE;
        }
    }

    // 九宫格有自己一整套键区（左标点列 + 3×3 + 右功能列），不进下面那条"三行逐键"的路。
    // 只接管三个字母行那一段：功能行在它下面，仍要走下面那份几何（少了这一层范围判断，
    // 九宫格的命中会把整条功能行也吃掉，空格/回车就成了死键）。
    if (nine) {
        const int gTop = evkRowY(evkLetter0());
        const int gH = evkRowH(evkLetter0());
        if (y >= gTop && y < gTop + 3 * gH) return evkNineGridHit(x, y, w, gH, hit);
    }

    // 符号面板 / 数字面板：整条字母行带归面板，同样不进下面那条"三行逐键"的路。
    // 范围判断用行带（不是 3 个字母行）——数字面板是四行，只判三行会把最后一行漏掉。
    if (evkSymPage() || evkNumPage()) {
        const int bTop = evkBandTop(), bH = evkBandH();
        if (y >= bTop && y < bTop + bH)
            return evkNumPage() ? evkNumPanelHit(x, y, w, hit) : evkSymPanelHit(x, y, w, hit);
    }

    // 字母三行：逐键椭圆。
    {
        const char *const *rows = evkRows();
        const bool ambig = evkAmbig();
        for (int r = 0; r < 3; r++) {
            int ky = evkRowY(evkLetter0() + r), kh = evkRowH(evkLetter0() + r);
            if (y < ky || y >= ky + kh) continue;
            if (ambig) {
                const int kMaxAmbig = 8;
                int xs[kMaxAmbig], ws[kMaxAmbig];
                evkAmbigRowRects(r, w, keyW, xs, ws);
                IME &ime = IME::getInstance();
                for (int k = 0; k < IME::ambigRowKeys(r) && k < kMaxAmbig; k++) {
                    if (!evkInEllipse(x, y, xs[k], ky, ws[k], kh)) continue;
                    int g = IME::ambigRowGroup(r, k);
                    const char *letters = IME::ambigGroupLetters(g);
                    evkSetHit(hit, xs[k], ky, ws[k], kh, 0, 0, false, IME::ambigGroupLabel(g));
                    // 输入法没开中文（或处于英文态）时无从消歧，退化为该组首字母
                    if (!ime.active() || ime.english()) return evkFinishKey(letters[0]);
                    return IME::KEY_AMBIG_BASE + g;
                }
                return EVK_NONE;
            }
            if (r < 2) {
                const char *row = rows[r];
                int n = static_cast<int>(strlen(row));
                int kx = (w - n * keyW) / 2;
                for (int k = 0; k < n; k++) {
                    int kxx = kx + k * keyW;
                    if (evkHitKeyChar(x, y, kxx, ky, keyW, kh, row[k], hit))
                        return evkFinishKey(static_cast<unsigned char>(row[k]));
                }
                return EVK_NONE;
            }
            // 第三行：字母页是 9 格 zxcvbnm + ，。。(标点键返回半角 ',' '.'——IME 在
            // 中文态自己会转成全角)。符号/数字面板在前面就被接走了，到不了这里。
            {
                int l3x = (w - 9 * keyW) / 2;    // 与 asdfghjkl 同一居中基准
                bool cn = evkCnPunct();
                for (int k = 0; k < 9; k++) {
                    int kxx = l3x + k * keyW;
                    if (!evkInEllipse(x, y, kxx, ky, keyW, kh)) continue;
                    if (k == 7) {
                        evkSetHit(hit, kxx, ky, keyW, kh, 0, 0, false, cn ? "，" : ",");
                        return evkFinishKey(',');
                    }
                    if (k == 8) {
                        evkSetHit(hit, kxx, ky, keyW, kh, 0, 0, false, cn ? "。" : ".");
                        return evkFinishKey('.');
                    }
                    if (evkHitKeyChar(x, y, kxx, ky, keyW, kh, EVK_LETTER_ROW3_MID[k], hit))
                        return evkFinishKey(static_cast<unsigned char>(EVK_LETTER_ROW3_MID[k]));
                }
                return EVK_NONE;
            }
        }
    }

    // 功能行：①面板切换 | ②面板切换 | 空格 | 中/英 | 回车。命中区间与绘制共用
    // evkFuncRowRects，两键之间没有空隙，也就没有"按了没反应"的死区(角上的死区由椭圆给出)。
    {
        int by = evkRowY(evkFuncRow()), bh = evkRowH(evkFuncRow());
        if (y >= by && y < by + bh) {
            int fx[5], fw[5];
            evkFuncRowRects(w, keyW, fx, fw);
            IME &ime = IME::getInstance();
            bool en = ime.english();
            // ①②：命中测试里就把面板切好（和布局键同一套路），调用方只需重绘。
            // 三个面板绕一圈，见 evkPanelKeyLabel 的注释。
            if (evkInEllipse(x, y, fx[0], by, fw[0], bh)) {
                evkSetHit(hit, fx[0], by, fw[0], bh, 0, 0, false, evkPanelKeyLabel(0));
                s_page = evkNumPage() ? 1 : (evkSymPage() ? 0 : 1);
                return EVK_PAGE;
            }
            if (evkInEllipse(x, y, fx[1], by, fw[1], bh)) {
                evkSetHit(hit, fx[1], by, fw[1], bh, 0, 0, false, evkPanelKeyLabel(1));
                s_page = evkNumPage() ? 0 : 2;
                return EVK_PAGE;
            }
            if (evkInEllipse(x, y, fx[2], by, fw[2], bh)) {
                evkSetHit(hit, fx[2], by, fw[2], bh, 5, 0, false, nullptr);
                return evkFinishKey(' ');
            }
            if (evkInEllipse(x, y, fx[3], by, fw[3], bh)) {
                const char *lang = !ime.active() ? "中" : (en ? "英" : "拼");
                evkSetHit(hit, fx[3], by, fw[3], bh, 0, 0, en, lang);
                return EVK_LANG;
            }
            if (evkInEllipse(x, y, fx[4], by, w - fx[4], bh)) {
                evkSetHit(hit, fx[4], by, w - fx[4], bh, 6, 0, false, nullptr);
                return evkFinishKey('\n');
            }
            return EVK_NONE;
        }
    }
    return EVK_NONE;
}

// 符号面板命中：和字母页同一套格子（10 列 × 3 行，见 EVK_SYM_PANEL），逐键椭圆。
// 返回普通 ASCII —— 中文态下 IME 自己会把 , . ? ! : ; 转成全角，与字母页的标点键同路。
static int evkSymPanelHit(int x, int y, int w, EditorVkHit *hit) {
    const int keyW = w / 10;
    const int x0 = evkRow3X(w, keyW);
    for (int r = 0; r < 3; r++) {
        const int ky = evkRowY(evkLetter0() + r), kh = evkRowH(evkLetter0() + r);
        if (y < ky || y >= ky + kh) continue;
        const char *row = EVK_SYM_PANEL[r];
        for (int k = 0; row[k]; k++) {
            const int kxx = x0 + k * keyW;
            if (evkHitKeyChar(x, y, kxx, ky, keyW, kh, row[k], hit))
                return evkFinishKey(static_cast<unsigned char>(row[k]));
        }
        return EVK_NONE;
    }
    return EVK_NONE;
}

// 数字面板命中：左列 | 中间三列(末行 0 横跨) | 右列(退格是图标键)。几何与
// evkNumPanelDraw 共用 evkNumRowY/evkNumRowH/evkNumSideW，两边不会算错位。
static int evkNumPanelHit(int x, int y, int w, EditorVkHit *hit) {
    const int side = evkNumSideW(w);
    const int colW = (w - 2 * side) / 3;
    for (int r = 0; r < 4; r++) {
        const int ry = evkNumRowY(r), rh = evkNumRowH(r);
        if (y < ry || y >= ry + rh) continue;
        if (evkInEllipse(x, y, 0, ry, side, rh)) {                 // 左列
            char lk[2] = {EVK_NUM_LEFT[r], 0};
            evkSetHit(hit, 0, ry, side, rh, 0, 0, false, lk);
            return evkFinishKey(static_cast<unsigned char>(EVK_NUM_LEFT[r]));
        }
        if (r < 3) {                                               // 中间三列：1..9
            for (int c = 0; c < 3; c++) {
                const int cx = side + c * colW;
                if (!evkInEllipse(x, y, cx, ry, colW, rh)) continue;
                char dk[2] = {static_cast<char>('1' + r * 3 + c), 0};
                evkSetHit(hit, cx, ry, colW, rh, 0, 0, false, dk);
                return evkFinishKey(static_cast<unsigned char>(dk[0]));
            }
        } else if (evkInEllipse(x, y, side, ry, colW * 3, rh)) {   // 末行：0 横跨三列
            evkSetHit(hit, side, ry, colW * 3, rh, 0, 0, false, "0");
            return evkFinishKey('0');
        }
        if (r == 0) {                                              // 右列第一格：退格
            if (evkInEllipse(x, y, w - side, ry, side, rh)) {
                evkSetHit(hit, w - side, ry, side, rh, 4, 0, false, nullptr);
                return evkFinishKey('\b');
            }
        } else {
            if (evkInEllipse(x, y, w - side, ry, side, rh)) {
                char rk[2] = {EVK_NUM_RIGHT[r], 0};
                evkSetHit(hit, w - side, ry, side, rh, 0, 0, false, rk);
                return evkFinishKey(static_cast<unsigned char>(EVK_NUM_RIGHT[r]));
            }
        }
        return EVK_NONE;
    }
    return EVK_NONE;
}

// 候选行左右滑动翻页。抬手时 input.cpp 会把一次横滑补成一个离散的 KEY_LEFT/RIGHT
// （滑动不是点按，拿不到 input_tap_xy），按下点由 input_press_xy() 带出来。界面收到
// 左右键先问这里：起点落在候选行 → 翻页并吃掉那个键；否则返回 false，界面照旧处理
// （阅读页翻页、编辑器移光标、词典/设置切标签……）。
// 只看**起点**不看终点：手指从候选行划出去也算划候选——"按住哪一行就是操作哪一行"，
// 而且抬手时手指多半已经离开候选行了，按终点判会一半手势失效。
bool editorVkSwipePage(int x, int y, int dir) {
    // 识别带必须与 editorVkDraw 画出来的格子逐像素对齐 —— 那是界面字号下的几何，
    // 而本函数在编辑器正文作用域里被调到（见 screen_editor_handle），不钉就会按
    // 正文字号算候选行高，整条带子跟着正文字号上/下移。
    FontScope ui(FontRenderer::uiPxHeight());
    if (!s_visible) return false;
    int top = editorVkTop();
    if (y < top + evkCandRowH(0) || y >= top + evkCandH()) return false;   // 只认候选行
    if (x < 0 || x >= SCREEN_W) return false;
    IME &ime = IME::getInstance();
    if (dir > 0) ime.nextPage(); else ime.prevPage();   // 手指左划 = 下一页，同翻页约定
    // 吃不吃掉与"是否真的翻动了"无关：候选行上的横划本来就不该跑去翻书/移光标，
    // 只有一页时按下去没反应才是正常的。
    return true;
}

// T9 候选面板里的上下滑动。抬手时 input.cpp 会把一次竖滑补成一个离散的
// KEY_PAGE_UP/DOWN——在正文里那是滚一屏/翻一页，起点落在面板上时归面板：
//   左列分音节选择 → 一次滚一屏（音节选项多到一屏放不下时才有得滚，右边有滚动条）；
//   候选宫格   → 一次翻一页（和候选条上左右划翻页共用 pagePrev/pageNext）。
// 只看**起点**，理由同 editorVkSwipePage：抬手时手指多半已经离开面板了。
// 返回 true = 这一划被面板吃掉了，调用方必须把 KEY_PAGE_UP/DOWN 丢掉，否则同一划
// 还会顺手把书翻一页/把正文滚一屏。面板没展开时返回 false，上下滑照旧是翻页。
bool editorVkSwipeScroll(int x, int y, int dir) {
    // 同 editorVkSwipePage：手势识别带要与画出来的一致，钉回界面字号。
    FontScope ui(FontRenderer::uiPxHeight());
    if (!s_visible) return false;
    const int w = SCREEN_W;
    // 九宫格：组合中左列就是分音节选择列，上下滑滚它（和 T9 面板共用 s_t9CodeTop
    // 这一个滚动位置——两边不会同时出现）。面板展开时整块键区归面板，走下面那条路。
    if (!evkT9Active() && evkNineKey()) {
        int pw, cx, cw, rx, rw;
        evkNineCols(w, &pw, &cx, &cw, &rx, &rw);
        const int gy = evkRowY(evkLetter0());
        const int gh = 3 * evkRowH(evkLetter0());
        if (x >= 0 && x < pw && y >= gy && y < gy + gh) {
            if (!evkNineSylOn()) return false;   // 标点列上的竖划不是本键盘的事
            int n, rh, vis, first, last;
            evkNineSylWindow(gh, &n, &rh, &vis, &first, &last);
            if (n > vis) {   // 真有得滚才吃掉这一划（没得滚就让它去翻页/移光标）
                s_t9CodeTop += (dir > 0) ? vis : -vis;
                evkT9CodeFirst(n, vis);   // 夹回合法范围
                ESP_LOGI("EdVk", "nine syl scroll dir=%d x=%d lw=%d n=%d vis=%d top=%d",
                         dir, x, pw, n, vis, s_t9CodeTop);
                return true;
            }
        }
    }
    if (!evkT9Active()) return false;
    const int top = evkT9Top(), bot = evkT9Bot();
    if (x < 0 || x >= w || y < top || y >= bot) return false;
    IME &ime = IME::getInstance();
    const int n = (int)ime.ambigSyllables().size();
    const int keyW = w / 10;
    const int lw = evkT9LeftW(w, keyW, n);
    if (lw > 0 && x < lw) {   // 分音节选择列
        const int h = bot - top;
        const int rh = evkT9CodeRowH(h, n);
        const int vis = evkT9CodeVis(h, rh);
        if (n > vis) {        // 真有得滚才吃掉这一划
            // 一次翻一整页，不是一行：读数少的时候(比如 12 个读音、一屏 6 行)一行一行
            // 挪几乎看不出变化，用户会当成"滑不动"。整页翻和旁边候选宫格的翻页同感。
            const int before = s_t9CodeTop;
            s_t9CodeTop += (dir > 0) ? vis : -vis;
            evkT9CodeFirst(n, vis);   // 夹回合法范围
            ESP_LOGI("EdVk", "t9 code scroll dir=%d x=%d lw=%d n=%d vis=%d %d->%d",
                     dir, x, lw, n, vis, before, s_t9CodeTop);
            return true;
        }
        // 装得下 = 没得滚。以前这里仍然 return true，于是这一划被左列白吃掉：用户想翻
        // 候选、手却落在左边那条读音栏上(x<lw，横屏 lw≈145 占屏宽 1/5)，看着就是"滑不动"。
        // 现在落到下面按候选宫格翻页 —— 左列既然没东西可滚，这一划唯一的合理去处就是翻候选。
    }
    const int before = ime.currentPage();
    const bool moved = (dir > 0) ? ime.nextPage() : ime.prevPage();
    ESP_LOGI("EdVk", "t9 grid page dir=%d x=%d lw=%d n=%d page=%d->%d moved=%d",
             dir, x, lw, n, before, ime.currentPage(), (int)moved);
    return true;
}

// 按下反馈：只记下命中的键，不在这里推屏。反色由 editorVkDraw 在本轮重绘的最后
// 补画一次——反馈搭按键动作自己那次刷新的顺风车，零额外刷新。
// 之前这里是"反色 + ui_commit + 停 120ms"：每按一键刷两遍（这里一遍、动作重绘一
// 遍），实测"按一下闪一下"，正是要改掉的。
void editorVkMarkPressed(const EditorVkHit &hit) {
    if (hit.w <= 0 || hit.h <= 0) { s_pressedValid = false; return; }
    s_pressed = hit;
    s_pressedValid = true;
}

// 丢掉按下反馈。给"按下去会改变自己外观"的键（中/英、布局、翻页…）用：
// 命中测试回带的几何/文字是**按下前**的那一份，而动作本身已经把这个键重画成了
// 新样子，再拿旧的一份反色叠上去，就会新旧两个标签摞在一起（"拼"和"英"叠字，
// 要再按一个其它键才刷新——那个键的反馈把旧的盖掉了）。这些键在本次重绘里已经
// 是最终形态，直接取消补画即可。
void editorVkClearPressed() { s_pressedValid = false; }

std::string editorVkTruncateToWidth(const std::string &s, int maxWidth) {
    // 给状态栏文字截宽用的，按**界面字号**量 —— 调用方可能正在正文作用域里
    // （editorStatusBar），但状态栏那一行是界面字号，量法必须跟着它。
    FontScope ui(FontRenderer::uiPxHeight());
    if (maxWidth <= 0) return std::string();
    if (g_vk_font.textWidth(s.c_str()) <= maxWidth) return s;
    std::string out;
    int wsum = 0;
    const char *p = s.c_str();
    while (*p) {
        const char *next = p;
        uint32_t cp = FontRenderer::utf8Decode(next);
        if (cp == 0) break;
        int cw = g_vk_font.charWidth(cp);
        if (wsum + cw > maxWidth) break;
        wsum += cw;
        out.append(p, static_cast<size_t>(next - p));
        p = next;
    }
    return out;
}
