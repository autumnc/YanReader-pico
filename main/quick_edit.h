#pragma once

#include <string>

// 快捷编辑模式: 直接编辑 SD 卡根目录的 /sdcard/0.txt ~ /sdcard/9.txt。
// 由设置项 app_mode("quick") 开启, 重启后生效。
extern bool g_quickEdit;

bool quickEditInit();                 // 确保 0-9 共 10 个文件存在
int  quickEditIndex();                // 当前文件 0-9 (持久化, 重启续上次)
void quickEditSetIndex(int idx);
void quickEditNext();                 // 循环 +1 (9 -> 0)
void quickEditPrev();                 // 循环 -1 (0 -> 9)
std::string quickEditFilePath(int idx);
std::string quickEditLoad(int idx);
bool quickEditSave(int idx, const std::string &text, bool createHistory = true);
