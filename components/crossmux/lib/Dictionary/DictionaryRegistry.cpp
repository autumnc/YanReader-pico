#include "DictionaryRegistry.h"

#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <cstring>

#include "StringUtils.h"

namespace DictionaryRegistry {
namespace {

// Dictionaries are looked up in both roots, in order. The hidden variant
// lets users keep the folder out of the file browser (hidden by default,
// see FileBrowserActivity's showHiddenFiles check).
constexpr const char* DICT_ROOTS[] = {"/dictionaries", "/.dictionaries"};

// Find the single .idx stem inside one dictionary folder. Returns false when
// the folder holds no .idx or more than one distinct stem (ambiguous).
bool findStem(const char* folderPath, std::string& stemOut) {
  auto dir = Storage.open(folderPath);
  // 失败原因一律用 WRN 打出来：这条链路以前全是 DBG（默认日志级别下等于静默），
  // 用户看到的现象只有上层一句"找不到词典"，没法分辨是目录打不开、名字不对，
  // 还是缺 .dict。诊断期间宁多勿少。
  if (!dir) {
    LOG_WRN("DREG", "findStem(%s): open failed", folderPath);
    return false;
  }
  if (!dir.isDirectory()) {
    LOG_WRN("DREG", "findStem(%s): not a directory", folderPath);
    return false;
  }

  dir.rewindDirectory();
  char name[128];
  char foundStem[128];
  foundStem[0] = '\0';
  // 收集看过的条目名，好让日志直接说明"卡里到底放了什么"。
  std::string seen;
  for (auto entry = dir.openNextFile(); entry; entry = dir.openNextFile()) {
    entry.getName(name, sizeof(name));
    if (seen.size() < 400) {
      if (!seen.empty()) seen += ", ";
      seen += name;
      if (entry.isDirectory()) seen += "/";
    }
    // Skip macOS metadata files (AppleDouble resource forks)
    if (entry.isDirectory() || strncmp(name, "._", 2) == 0) continue;

    const size_t len = strlen(name);
    if (len <= 4 || strcmp(name + len - 4, ".idx") != 0) continue;

    name[len - 4] = '\0';
    if (foundStem[0] != '\0' && strcmp(foundStem, name) != 0) {
      LOG_WRN("DREG", "findStem(%s): multiple index stems ('%s' vs '%s')", folderPath, foundStem, name);
      return false;
    }
    strncpy(foundStem, name, sizeof(foundStem) - 1);
    foundStem[sizeof(foundStem) - 1] = '\0';
  }

  if (foundStem[0] == '\0') {
    LOG_WRN("DREG", "findStem(%s): no *.idx here; entries=[%s]", folderPath, seen.c_str());
    return false;
  }

  // Require dictionary data next to the index, so folders holding only an
  // .idx never surface as selectable dictionaries that fail at lookup time.
  const std::string base = std::string(folderPath) + "/" + foundStem;
  if (!Storage.exists((base + ".dict").c_str()) && !Storage.exists((base + ".dict.dz").c_str())) {
    LOG_WRN("DREG", "findStem(%s): stem '%s' has no .dict/.dict.dz; entries=[%s]", folderPath, foundStem,
            seen.c_str());
    return false;
  }

  stemOut = foundStem;
  return true;
}

}  // namespace

void discover(std::vector<DictionaryEntry>& out) {
  out.clear();
  out.reserve(8);

  for (const char* dictRoot : DICT_ROOTS) {
    auto rootDir = Storage.open(dictRoot);
    if (!rootDir || !rootDir.isDirectory()) {
      LOG_DBG("DREG", "No %s directory on SD card", dictRoot);
      continue;
    }

    rootDir.rewindDirectory();
    char name[128];
    for (auto entry = rootDir.openNextFile(); entry; entry = rootDir.openNextFile()) {
      entry.getName(name, sizeof(name));
      if (!entry.isDirectory() || name[0] == '.') continue;

      std::string folderPath = std::string(dictRoot) + "/" + name;
      std::string stem;
      if (!findStem(folderPath.c_str(), stem)) continue;

      DictionaryEntry e;
      e.name = name;
      e.stem = std::move(stem);
      out.push_back(std::move(e));
      LOG_DBG("DREG", "Found dictionary: %s", name);
    }
  }

  // Case-insensitive sort by folder name (matches FileBrowserActivity ordering).
  std::sort(out.begin(), out.end(), [](const DictionaryEntry& a, const DictionaryEntry& b) {
    return StringUtils::asciiCaseCmp(a.name.c_str(), b.name.c_str()) < 0;
  });
}

bool resolveBasePath(const char* folderName, std::string& basePathOut) {
  if (!folderName || folderName[0] == '\0') return false;
  // "." = 词典不放进子目录，三件套直接摊在词典根目录下（手拷 StarDict 词典的人
  // 最常这么放）。此时"根目录"本身就是那本词典的文件夹，.idx 就在 /dictionaries/
  // 里。必须放在下面那条"以点开头的名字一律拒绝"之前。
  if (strcmp(folderName, ".") == 0) {
    for (const char* dictRoot : DICT_ROOTS) {
      std::string stem;
      if (!findStem(dictRoot, stem)) continue;
      basePathOut = std::string(dictRoot) + "/" + stem;
      return true;
    }
    return false;
  }
  // folderName is persisted in the settings JSON: reject separators and dot
  // prefixes so a crafted value cannot escape the dictionary roots.
  if (folderName[0] == '.' || strpbrk(folderName, "/\\") != nullptr) return false;

  for (const char* dictRoot : DICT_ROOTS) {
    std::string folderPath = std::string(dictRoot) + "/" + folderName;
    std::string stem;
    if (!findStem(folderPath.c_str(), stem)) continue;
    basePathOut = folderPath + "/" + stem;
    return true;
  }
  return false;
}

}  // namespace DictionaryRegistry
