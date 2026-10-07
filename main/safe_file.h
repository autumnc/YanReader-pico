#pragma once

#include <cstddef>
#include <cstdio>
#include <string>

static constexpr size_t READ_WHOLE_FILE_DEFAULT_MAX = 2 * 1024 * 1024;

bool safeWriteFile(const std::string &path, const std::string &content);

// 流式原子写：内容**不要求**先进内存，由 writeChunk 直接往 <path>.tmp 里追加；返回 false
// 表示放弃（本次不提交）。提交语义与 safeWriteFile 完全一致：.tmp → fsync → rename
// （原文件先退到 .bak，失败回滚）。给"表的大小没有上界"的调用方用 —— 那种表拼成一整份
// std::string 会在 -fno-exceptions 下因分配失败直接 abort()。
// writeChunk 返回 false、短写、fsync 报错，都走同一条回滚：删 .tmp，盘上仍是上一份完整
// 内容。ctx 原样透传，可为 nullptr。
bool safeWriteFileStream(const std::string &path,
                         bool (*writeChunk)(FILE *tmp, void *ctx),
                         void *ctx);
void repairSafeWriteFile(const std::string &path);
std::string readWholeFile(const std::string &path,
                          size_t maxBytes = READ_WHOLE_FILE_DEFAULT_MAX,
                          bool *truncated = nullptr);
bool fileExists(const std::string &path);
bool ensureDirPath(const std::string &path);
