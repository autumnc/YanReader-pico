#pragma once

#include <string>

// 三模式共享的「输入框触摸编辑」——和 editor_vk / edit_menu 一样是底层件：
// 模块持有会话状态、只管几何/命中/绘制，动作落在共享的 clipboard 上。
//
// 覆盖的是**扁平缓冲 + 一行行画出来**的输入框：计划模式的输入框、灵感的关键词与
// 检索框、flomo 的检索框、设置界面的文本框、润色提示词、计划模式的备注编辑器。
// 它们都能说清"缓冲里第 [start,end) 个字节画在屏幕的哪一行、从哪个 x 起"，所以
// 宿主把这张**行表**交给模块即可，模块不必知道宿主是 std::string 还是
// std::vector<std::string>、是折行还是换行。
//
// 编辑器正文不走这里（它有折行 / markdown / 竖排 / 两柄拖选，自己在
// screen_editor.cpp 里实现），但两边**共用同一份 clipboard 与同一个 edit_menu
// 控件**：这正是"同虚拟键盘一样"的底层件的含义——触摸与实体键盘落到同一份机制上。
//
// 用法（宿主）：
//   1. 画完字段本体后调 textSelDraw(buf, view)；
//   2. 长按（KEY_TOUCH_LONG）落在某一行上 → textSelBegin(...)，返回 true 就吃掉；
//   3. 其它键在"自己的 key 分支之前、喂 IME 之前"调 textSelHandleKey(...)，
//      返回 true 就 return。返回 false 表示这一键不归会话管，照常处理。
//   4. 离开字段 / 切模式 / 提交 / 取消时 textSelReset()。

// 一行显示行：缓冲里 [start, end) 这一段从屏幕 (x0, baseline) 起画。
struct TextSelLine {
    int start;      // 该行首字节在 buf 里的偏移
    int end;        // 该行末字节（不含）
    int x0;         // 行文本左端屏幕 x
    int baseline;   // 该行基线 y
};

// 一次交给模块的几何。lines 指向宿主构造的行表（每帧重算，模块不持有）。
struct TextSelView {
    const TextSelLine *lines = nullptr;
    int count = 0;
    int bottom = 0;   // 按钮条贴在这个 y 之上画（虚拟键盘/候选条上沿）；<=0 = 屏底
};

// 进入/离开字段时清一次（切模式、提交、取消、切走）。
void textSelReset();
bool textSelActive();

// 长按：选中手指底下那一整行（含行尾换行），弹「复制 / 剪切 / 粘贴 / 全选」。
// 落点不落在任何一行上返回 false（交给宿主的老语义，比如"长按=返回"）。
// 返回 true = 这一下归会话；会话已经开着时再长按 = 收掉。
bool textSelBegin(std::string &buf, int &cur, const TextSelView &view, int x, int y);

// 会话里的一键。返回 true = 已消费；false = 放行（调用方照常处理这一键）。
// buf / cur 是宿主的缓冲与字节光标，模块可能**直接改**它们。
// (tapX, tapY, hasTap) 是**宿主已经取好的**本次点按落点：模块不自己去 input_tap_xy
// （那是一次性接口，宿主的键盘泵/坐标分流已经读过一遍了，再读是空的）。
// status 非空时把提示语（"已复制"…）回给宿主的状态行。
bool textSelHandleKey(std::string &buf, int &cur, const TextSelView &view, int key,
                      int tapX, int tapY, bool hasTap, std::string *status);

// 宿主画完字段本体之后调：反白选区 + 按钮条 + 粘贴板列表。
void textSelDraw(const std::string &buf, const TextSelView &view);
