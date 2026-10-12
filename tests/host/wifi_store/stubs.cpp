// 主机测试的替身：wifi_store.cpp 的**纯函数**（serialize/parse/escapeField/
// unescapeField）不碰文件系统，但同一个 .o 里的 load()/save() 会引用读盘、写盘与
// g_settings。静态链接把整个 .o 拉进来，所以那几个符号必须有人提供 —— 这里给空实现。
// 一律不参与测例断言，被测的只有纯函数（见 regress.cpp 的头注释）。
#include "safe_file.h"
#include "settings_manager.h"

#include <cstdio>
#include <string>

SettingsManager g_settings;

// wifi_store.cpp 的 load() 只读这两个键（旧键迁移），测试里永远给空 —— 迁移那条路
// 由 load() 自己覆盖，纯函数不经过它。
std::string SettingsManager::wifiSsid() { return {}; }
std::string SettingsManager::wifiPassword() { return {}; }

// 读盘：永远"文件不存在"，于是 load() 走空表 + 空迁移那条路。
std::string readWholeFile(const std::string &, size_t, bool *) { return {}; }

// 写盘：报告成功，但什么也不落（测例从不调用 save）。
bool safeWriteFile(const std::string &, const std::string &) { return true; }
bool safeWriteFileStream(const std::string &, bool (*)(FILE *, void *), void *) { return true; }
void repairSafeWriteFile(const std::string &) {}
bool fileExists(const std::string &) { return false; }
bool ensureDirPath(const std::string &) { return true; }
