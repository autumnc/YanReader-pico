#pragma once
// ── 文本字段绑定（ImeField）─────────────────────────────────────────────
// 一个"可编辑字段" = 一份串 + 一个字节偏移的光标。宿主把**自己那份**串和光标指针交
// 进来（不拷贝、不接管所有权），这里只做 UTF-8 安全的算术：落串、退格、光标左右移。
//
// 为什么值得单独一层：这套算术原本在三个宿主里各写了一遍，而且**已经漂了**——
//   · GTD 的退格有 `prev--` 和 `cur--` 两种写法（screen_gtd.cpp 各 mode 一份）；
//   · 阅读模式的 popUtf8 是第三种；编辑器的 utf8Prev 是第四种；
//   · "输入法优先，没接住的可打印字符当裸字符落串"这条规矩，GTD / 阅读器 / 编辑器
//     三份实现互不相同（阅读模式少了这一段时数字面板一个数字都打不出来）。
// 输入法相关的 bug 大多出在这一层，所以这里是"一处修、处处修"最划算的地方。
//
// **钩子（撤销/脏标记/重绘/滚动）不放进来**：读遍三个宿主，这些动作全都发生在落串
// 的**调用点之外**（编辑器有自己的 editorInsertText 漏斗、GTD 在返回前 drawAdd()、
// 阅读器把 st.dirty 交给调用方），而且一个宿主一个样（编辑器要跨行合行、阅读器要重算
// 搜索命中）。硬塞进来只会多一层没人用的间接。宿主要的就是下面这几个纯算术原语。
//
// 长度上限（评审里提过的 maxBytes）**没有做**：全仓现在没有任何字段设过长度上限，
// 加了就是死配置。真要限的时候在调用点判断即可。
#include <string>

class IME;

struct ImeField {
    std::string *text = nullptr;  // 目标串；nullptr = 当前没有目标（键被吃掉但不落串）
    int *cursor = nullptr;        // 光标（字节偏移）；nullptr = 只追加，光标恒在串尾

    bool valid() const { return text != nullptr; }
    int length() const { return text ? (int)text->size() : 0; }
    int cur() const { return cursor ? *cursor : length(); }
    void setCur(int c) {
        if (!cursor) return;
        const int n = length();
        *cursor = (c < 0) ? 0 : (c > n ? n : c);
    }
};

// ── 原语：纯算术，不碰输入法 ────────────────────────────────────────────
// 返回 true = 真的改了串/动了光标；false = 已经在边界上，什么都没发生。
// 光标为 nullptr（追加型字段）时，"退格"就是删串尾那个码点。
// ImeField 按**值**传：它只有两个指针，拷的是指针不是串；这样宿主可以直接把
// "当前字段是哪个"写成一个小函数（`imeFieldBackspace(editorLineField())`）。
//
// 前删（Delete）**没做**：全仓没有任何键码会走到"删光标后一个码点"，同 maxBytes。
bool imeFieldInsert(ImeField f, const std::string &s);  // 落在光标处，光标推到插入段末尾
bool imeFieldBackspace(ImeField f);                     // 删光标前一个码点
bool imeFieldMoveLeft(ImeField f);                      // 光标左移一个码点
bool imeFieldMoveRight(ImeField f);                     // 光标右移一个码点
void imeFieldMoveEnd(ImeField f);                       // 光标到串尾

// ── 喂键：只产出"该落什么"，不落串 ─────────────────────────────────────
// 输入法优先解析一个键：
//   输入法接住 → 返回 true，out = 它吐出来的那一段（**可能是空串**：翻页/选词/清组合/
//                进了删除模式，这些都算"键被吃了但没有新文本"）；
//   没接住 → 键是可打印 ASCII 就当裸字符（返回 true，out = 那一个字符）；
//   都不是 → 返回 false，调用方按自己的规矩处理这个键。
// multiline=false 时把纯 '\n' 的输出吃掉（换行语义归宿主）。
//
// **为什么不顺手落串**：三个宿主落串时都还带别的副作用 —— 编辑器要记撤销快照、
// 标脏、排自动保存；GTD 要标 noteVrowsDirty；阅读器要重置搜索选中项。让这个函数
// 直接改串，就必须再给它一套宿主钩子，而这些副作用的位置在各自文件里本来就清清楚楚。
// 所以这里只回答"该落什么"，落哪儿由调用方用 imeFieldInsert 或自己的漏斗决定。
bool imeFieldKeyText(IME &ime, int key, bool multiline, std::string &out);
