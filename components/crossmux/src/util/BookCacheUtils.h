#pragma once
#include <string>

// 清掉某本书在阅读器里的缓存目录（封面/章节排版缓存）。
// 微信读书用部分章节范围重新打包 EPUB 时，旧缓存必须作废，否则阅读器会沿用旧书。
// 路径不认识的返回 true（无事可做）。原版还带改名/清理整库的辅助，
// 本移植只保留这一条被 WeReadWebApi 调用的入口。
bool clearBookCache(const std::string& path);
