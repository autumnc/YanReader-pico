#pragma once

#include "ime_config.h"

#include <string>
#include <vector>

namespace ime {

struct PinyinToken {
    std::string text;
    bool partial = false;
};

struct PinyinSplit {
    std::vector<PinyinToken> tokens;
    int score = 0;
};

class PinyinEngine {
public:
    static bool enabled();
    static bool isCodeChar(char c);
    static std::string normalize(const std::string &code);
    static std::string removeSplit(const std::string &code);

    static bool isValidSyllable(const std::string &s);
    static bool isSyllablePrefix(const std::string &s);
    static bool isValidCode(const std::string &code);
    // 音节表的枚举入口。分音节选择（14/9/18 键的一键多字母歧义编码）要"从一个键窗口
    // 能拼出哪些音节"，正着枚举字母组合的代价随每键字母数指数涨（9 键每键 3-4 个），
    // 反过来扫这 409 条音节、逐条比对键窗口则是常数代价，而且与键位分组无关——
    // 换布局不用改这段。
    static int syllableCount();
    static const char *syllableAt(int i);

    static PinyinSplit primarySplit(const std::string &code, bool allowPartial);
    static std::vector<PinyinSplit> splitVariants(const std::string &code,
                                                  bool allowPartial,
                                                  int maxVariants = 8);
    static std::vector<int> prefixMatchLengths(const std::string &code);
    static std::string singleKeyFallbackSyllable(const std::string &code);
    static std::string initialCode(const std::string &code);
};

} // namespace ime
