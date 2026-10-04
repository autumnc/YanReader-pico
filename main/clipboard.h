#pragma once

#include <string>

// 跨模式共享粘贴板（写作 / 阅读 / 计划三模式共用的底层件，同虚拟键盘）。
// 多条历史 + 落盘，重启后还在。
//
// 存 /sdcard/settings/clipboard.txt，一条一行；条目里的换行转义成 "\n"、反斜杠转义成
// "\\"（与阅读器笔记 reader_notes.txt 是同一套"一行一条 + 转义"的思路，见
// screen_reader.cpp 的 rdNoteEscape）。上限：kClipboardMax 条、每条 kClipboardItemMax
// 字节，超出裁掉最旧的。
//
// **惰性加载**：第一次访问时读盘，所以在任何时点调用都安全（SD 还没挂载时读到空）。
// main.cpp 在 SD 挂载后显式调一次 clipboardLoad() 把它读进来。

static const int kClipboardMax = 10;
static const int kClipboardItemMax = 4096;

int clipboardCount();
const std::string &clipboardAt(int i);  // i=0 是最新的一条；越界返回空串
std::string clipboardLatest();          // 空粘贴板返回 ""
bool clipboardEmpty();
// 空串忽略；同内容提到最前；立即落盘（复制是低频动作，不必攒）。
void clipboardPush(const std::string &text);
void clipboardClear();
void clipboardLoad();                   // 启动时调一次（SD 挂载后）
bool clipboardLastTruncated();          // 上一次 push 是否因超长被截断
