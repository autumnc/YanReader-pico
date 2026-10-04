#pragma once

// 文件小工具（移植自 ../Flomo/src/safe_file.{h,cpp}）。
// 写文件走 "临时文件 → 备份 → 改名" 三步：Flomo 的 memos.json 是本地唯一的笔记库，
// 写一半掉电不能把库写坏——改名在 FatFS 上是原子的。
// 路径一律是 SD 卡原生态绝对路径（/sdcard/...），与 journal_storage 一致。

#include <string>

bool flomoFileExists(const std::string &path);
bool flomoEnsureDir(const std::string &path);
std::string flomoReadWholeFile(const std::string &path);
bool flomoSafeWriteFile(const std::string &path, const std::string &content);
void flomoRepairFile(const std::string &path);
