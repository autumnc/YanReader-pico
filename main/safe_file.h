#pragma once

#include <cstddef>
#include <string>

static constexpr size_t READ_WHOLE_FILE_DEFAULT_MAX = 2 * 1024 * 1024;

bool safeWriteFile(const std::string &path, const std::string &content);
void repairSafeWriteFile(const std::string &path);
std::string readWholeFile(const std::string &path,
                          size_t maxBytes = READ_WHOLE_FILE_DEFAULT_MAX,
                          bool *truncated = nullptr);
bool fileExists(const std::string &path);
bool ensureDirPath(const std::string &path);
