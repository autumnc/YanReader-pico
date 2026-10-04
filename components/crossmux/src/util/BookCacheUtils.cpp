#include "BookCacheUtils.h"

#include <Epub.h>
#include <Logging.h>
#include <hal/HalStorage.h>

#include <functional>

// 与 screen_reader.cpp 的 bookCacheDirFor() 必须保持一致：缓存根目录 + 类型前缀 + 路径哈希。
static const char* kCacheRoot = "/sdcard/.crossmux";

bool clearBookCache(const std::string& path) {
  if (path.size() < 5) return true;
  std::string low;
  for (char c : path) low += (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
  const char* pfx = nullptr;
  if (low.compare(low.size() - 5, 5, ".epub") == 0) pfx = "epub";
  else if (low.compare(low.size() - 4, 4, ".txt") == 0) pfx = "txt";
  else if (low.compare(low.size() - 4, 4, ".xtc") == 0) pfx = "xtc";
  if (!pfx) return true;
  const std::string dir =
      std::string(kCacheRoot) + "/" + pfx + "_" + std::to_string(std::hash<std::string>{}(path));
  if (!Storage.exists(dir.c_str())) return true;
  LOG_INF("WR", "清除阅读缓存 %s", dir.c_str());
  return Storage.removeDir(dir.c_str());
}
