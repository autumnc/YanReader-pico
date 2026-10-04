#include "yong_pinyin.h"

#include <algorithm>
#include <cstring>

namespace ime {
namespace {

static const char *const kSyllables[] = {
    "a","ai","an","ang","ao",
    "ba","bai","ban","bang","bao","bei","ben","beng","bi","bian","biao","bie","bin","bing","bo","bu",
    "ca","cai","can","cang","cao","ce","cen","ceng","cha","chai","chan","chang","chao","che","chen","cheng",
    "chi","chong","chou","chu","chua","chuai","chuan","chuang","chui","chun","chuo","ci","cong","cou","cu",
    "cuan","cui","cun","cuo",
    "da","dai","dan","dang","dao","de","dei","den","deng","di","dia","dian","diao","die","ding","diu",
    "dong","dou","du","duan","dui","dun","duo",
    "e","ei","en","eng","er",
    "fa","fan","fang","fei","fen","feng","fo","fou","fu",
    "ga","gai","gan","gang","gao","ge","gei","gen","geng","gong","gou","gu","gua","guai","guan","guang",
    "gui","gun","guo",
    "ha","hai","han","hang","hao","he","hei","hen","heng","hong","hou","hu","hua","huai","huan","huang",
    "hui","hun","huo",
    "ji","jia","jian","jiang","jiao","jie","jin","jing","jiong","jiu","ju","juan","jue","jun",
    "ka","kai","kan","kang","kao","ke","kei","ken","keng","kong","kou","ku","kua","kuai","kuan","kuang",
    "kui","kun","kuo",
    "la","lai","lan","lang","lao","le","lei","leng","li","lia","lian","liang","liao","lie","lin","ling",
    "liu","lo","long","lou","lu","luan","lun","luo","lv","lve",
    "ma","mai","man","mang","mao","me","mei","men","meng","mi","mian","miao","mie","min","ming","miu",
    "mo","mou","mu",
    "na","nai","nan","nang","nao","ne","nei","nen","neng","ni","nian","niang","niao","nie","nin","ning",
    "niu","nong","nou","nu","nuan","nun","nuo","nv","nve",
    "o","ou",
    "pa","pai","pan","pang","pao","pei","pen","peng","pi","pian","piao","pie","pin","ping","po","pou","pu",
    "qi","qia","qian","qiang","qiao","qie","qin","qing","qiong","qiu","qu","quan","que","qun",
    "ran","rang","rao","re","ren","reng","ri","rong","rou","ru","ruan","rui","run","ruo",
    "sa","sai","san","sang","sao","se","sen","seng","sha","shai","shan","shang","shao","she","shen",
    "sheng","shi","shou","shu","shua","shuai","shuan","shuang","shui","shun","shuo","si","song","sou",
    "su","suan","sui","sun","suo",
    "ta","tai","tan","tang","tao","te","tei","teng","ti","tian","tiao","tie","ting","tong","tou","tu",
    "tuan","tui","tun","tuo",
    "wa","wai","wan","wang","wei","wen","weng","wo","wu",
    "xi","xia","xian","xiang","xiao","xie","xin","xing","xiong","xiu","xu","xuan","xue","xun",
    "ya","yan","yang","yao","ye","yi","yin","ying","yo","yong","you","yu","yuan","yue","yun",
    "za","zai","zan","zang","zao","ze","zei","zen","zeng","zha","zhai","zhan","zhang","zhao","zhe",
    "zhen","zheng","zhi","zhong","zhou","zhu","zhua","zhuai","zhuan","zhuang","zhui","zhun","zhuo",
    "zi","zong","zou","zu","zuan","zui","zun","zuo",
};

static const size_t kSyllableCount = sizeof(kSyllables) / sizeof(kSyllables[0]);

static bool hasExplicitSplit(const std::string &code) {
    return code.find('\'') != std::string::npos;
}

static bool splitExplicit(const std::string &code, bool allowPartial, PinyinSplit &out) {
    out.tokens.clear();
    out.score = 0;
    std::string part;
    for (size_t i = 0; i <= code.size(); i++) {
        if (i < code.size() && code[i] != '\'') {
            part += code[i];
            continue;
        }
        if (part.empty()) return false;
        bool exact = PinyinEngine::isValidSyllable(part);
        if (!exact && !(allowPartial && PinyinEngine::isSyllablePrefix(part))) return false;
        out.tokens.push_back({part, !exact});
        out.score += exact ? 8 : 1;
        part.clear();
    }
    return !out.tokens.empty();
}

static void enumerateSplits(const std::string &code, size_t pos, bool allowPartial,
                            PinyinSplit &cur, std::vector<PinyinSplit> &out,
                            int maxVariants) {
    if ((int)out.size() >= maxVariants) return;
    if (pos >= code.size()) {
        out.push_back(cur);
        return;
    }
    // 两处循环都要判 exact, 但 isValidSyllable 是对 409 条 kSyllables(flash rodata)的二分,
    // 每键要以千次计; 第一轮算出来就得存住, 别在第二轮重算一遍。
    int bestLens[8];
    bool bestExact[8];
    int bestCount = 0;
    int maxLen = std::min<int>(6, (int)code.size() - (int)pos);
    for (int len = maxLen; len >= 1; len--) {
        std::string part = code.substr(pos, len);
        bool exact = PinyinEngine::isValidSyllable(part);
        bool partial = !exact && allowPartial && (pos + len == code.size()) &&
                       PinyinEngine::isSyllablePrefix(part);
        if (!exact && !partial) continue;
        bestExact[bestCount] = exact;
        bestLens[bestCount++] = len;
        if (bestCount >= (int)(sizeof(bestLens) / sizeof(bestLens[0]))) break;
    }
    for (int i = 0; i < bestCount; i++) {
        int len = bestLens[i];
        bool exact = bestExact[i];
        std::string part = code.substr(pos, len);
        cur.tokens.push_back({part, !exact});
        cur.score += exact ? (len * 8 + (len >= 2 ? 2 : -4)) : len;
        enumerateSplits(code, pos + len, allowPartial, cur, out, maxVariants);
        cur.score -= exact ? (len * 8 + (len >= 2 ? 2 : -4)) : len;
        cur.tokens.pop_back();
    }
}

static std::string tokensToCode(const std::vector<PinyinToken> &tokens, bool split) {
    std::string out;
    for (size_t i = 0; i < tokens.size(); i++) {
        if (split && i > 0) out += '\'';
        out += tokens[i].text;
    }
    return out;
}

static std::string initialOf(const std::string &syllable) {
    if (syllable.size() >= 2) {
        std::string two = syllable.substr(0, 2);
        if (two == "zh" || two == "ch" || two == "sh") return two;
    }
    char c = syllable.empty() ? 0 : syllable[0];
    if (std::strchr("bpmfdtnlgkhjqxrzcsyw", c)) return std::string(1, c);
    return syllable.empty() ? std::string() : syllable.substr(0, 1);
}

} // namespace

bool PinyinEngine::enabled() {
    return PJOURNAL_IME_ENABLE_PINYIN != 0;
}

bool PinyinEngine::isCodeChar(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '\'';
}

std::string PinyinEngine::normalize(const std::string &code) {
    std::string out;
    out.reserve(code.size());
    for (char c : code) {
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if ((c >= 'a' && c <= 'z') || c == '\'') out += c;
    }
    return out;
}

std::string PinyinEngine::removeSplit(const std::string &code) {
    std::string out;
    out.reserve(code.size());
    for (char c : code) if (c != '\'') out += c;
    return out;
}

// kSyllables is strcmp-sorted (keep it that way when editing the table); both
// probes below binary-search it instead of scanning all 409 entries, which was
// the dominant cost of the split DFS on every keystroke.
static const char *const *lowerSyllable(const char *key) {
    const char *const *begin = kSyllables;
    const char *const *end = kSyllables + kSyllableCount;
    return std::lower_bound(begin, end, key,
                            [](const char *entry, const char *k) {
                                return std::strcmp(entry, k) < 0;
                            });
}

bool PinyinEngine::isValidSyllable(const std::string &s) {
    if (s.empty()) return false;
    const char *const *end = kSyllables + kSyllableCount;
    const char *const *it = lowerSyllable(s.c_str());
    return it != end && std::strcmp(*it, s.c_str()) == 0;
}

bool PinyinEngine::isSyllablePrefix(const std::string &s) {
    if (s.empty()) return false;
    const char *const *end = kSyllables + kSyllableCount;
    const char *const *it = lowerSyllable(s.c_str());
    // Every syllable starting with s sorts at or after s, so the first entry >= s
    // is a prefix match iff any match at all exists.
    return it != end && std::strncmp(*it, s.c_str(), s.size()) == 0;
}

int PinyinEngine::syllableCount() { return (int)kSyllableCount; }

const char *PinyinEngine::syllableAt(int i) {
    if (i < 0 || i >= (int)kSyllableCount) return "";
    return kSyllables[i];
}

bool PinyinEngine::isValidCode(const std::string &code) {
    PinyinSplit split = primarySplit(code, false);
    return !split.tokens.empty() && tokensToCode(split.tokens, hasExplicitSplit(code)) == code;
}

PinyinSplit PinyinEngine::primarySplit(const std::string &raw, bool allowPartial) {
    std::string code = normalize(raw);
    PinyinSplit empty;
    if (code.empty()) return empty;
    if (hasExplicitSplit(code)) {
        PinyinSplit out;
        return splitExplicit(code, allowPartial, out) ? out : empty;
    }
    std::vector<PinyinSplit> variants = splitVariants(code, allowPartial, 1);
    return variants.empty() ? empty : variants[0];
}

std::vector<PinyinSplit> PinyinEngine::splitVariants(const std::string &raw,
                                                     bool allowPartial,
                                                     int maxVariants) {
    std::vector<PinyinSplit> out;
    if (raw.empty() || maxVariants <= 0) return out;
    // Ring cache keyed on the raw code. A single keystroke re-requests the same code
    // several times (seg phase, then the fuzzy-alias phase) with identical
    // (allowPartial, maxVariants), and the DFS below dominates the keystroke cost.
    // The function is pure, so a hit is exact. Keys are compared against the raw
    // argument, so a hit also skips the normalize() allocation.
    struct CacheEntry {
        std::string code;
        bool allowPartial = false;
        int maxVariants = 0;
        bool valid = false;
        std::vector<PinyinSplit> result;
    };
    static const int kSlots = 8;
    static CacheEntry cache[kSlots];
    static int nextSlot = 0;
    for (int i = 0; i < kSlots; i++) {
        const CacheEntry &e = cache[i];
        if (e.valid && e.maxVariants == maxVariants && e.allowPartial == allowPartial &&
            e.code == raw) {
            return e.result;
        }
    }
    std::string code = normalize(raw);
    if (code.empty()) return out;
    if (hasExplicitSplit(code)) {
        PinyinSplit split;
        if (splitExplicit(code, allowPartial, split)) out.push_back(split);
        return out;
    }
    PinyinSplit cur;
    enumerateSplits(code, 0, allowPartial, cur, out, maxVariants);
    std::stable_sort(out.begin(), out.end(), [](const PinyinSplit &a, const PinyinSplit &b) {
        if (a.score != b.score) return a.score > b.score;
        return a.tokens.size() < b.tokens.size();
    });
    CacheEntry &slot = cache[nextSlot];
    nextSlot = (nextSlot + 1) % kSlots;
    slot.code = raw;
    slot.allowPartial = allowPartial;
    slot.maxVariants = maxVariants;
    slot.result = out;
    slot.valid = true;
    return out;
}

std::vector<int> PinyinEngine::prefixMatchLengths(const std::string &raw) {
    std::string code = normalize(raw);
    std::vector<int> out;
    int maxLen = std::min<int>(6, code.size());
    for (int len = maxLen; len >= 1; len--) {
        std::string prefix = code.substr(0, len);
        if (isValidSyllable(prefix) || isSyllablePrefix(prefix)) out.push_back(len);
    }
    if (out.empty() && !code.empty()) out.push_back(1);
    return out;
}

std::string PinyinEngine::singleKeyFallbackSyllable(const std::string &raw) {
    std::string code = normalize(raw);
    if (code.size() == 1) {
        switch (code[0]) {
        case 'b': return "bu";
        case 'c': return "ci";
        case 'd': return "de";
        case 'f': return "fei";
        case 'g': return "ge";
        case 'h': return "he";
        case 'j': return "ji";
        case 'k': return "ke";
        case 'l': return "le";
        case 'm': return "mei";
        case 'n': return "ni";
        case 'p': return "pai";
        case 'q': return "qi";
        case 'r': return "ren";
        case 's': return "san";
        case 't': return "ta";
        case 'w': return "wo";
        case 'x': return "xiao";
        case 'y': return "yi";
        case 'z': return "zai";
        default: return "";
        }
    }
    if (code.size() == 2 && code[1] == 'h') {
        switch (code[0]) {
        case 'c': return "chu";
        case 's': return "shi";
        case 'z': return "zhe";
        default: return "";
        }
    }
    return "";
}

std::string PinyinEngine::initialCode(const std::string &raw) {
    std::string out;
    std::string code = normalize(raw);
    if (code.empty()) return out;
    size_t pos = 0;
    while (pos < code.size()) {
        if (code[pos] == '\'') {
            pos++;
            continue;
        }
        int bestLen = 0;
        int maxLen = std::min<int>(6, (int)code.size() - (int)pos);
        for (int len = maxLen; len >= 1; len--) {
            std::string part = code.substr(pos, len);
            if (isValidSyllable(part)) {
                bestLen = len;
                break;
            }
        }
        if (bestLen <= 0) return "";
        out += initialOf(code.substr(pos, bestLen));
        pos += bestLen;
    }
    return out;
}

} // namespace ime
