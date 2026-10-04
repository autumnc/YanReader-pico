#pragma once

#include <string>

bool safeWriteFile(const std::string &path, const std::string &content);
void repairSafeWriteFile(const std::string &path);
std::string readWholeFile(const std::string &path);
bool fileExists(const std::string &path);
bool ensureDirPath(const std::string &path);
