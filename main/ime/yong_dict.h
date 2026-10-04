#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include "ime_config.h"

namespace ime {

class Im3Dictionary {
public:
    enum Scheme : uint8_t { WUBI = 0, PINYIN = 1, SHUANGPIN = 2 };

    static const int kHeaderSize = 12;
    static const int kIndexEntries = 26 * 26 + 1;
    static const int kMaxCodeLen = 6;
    static const int kHanziSize = 3;
    static const int kFlagSize = 1;

    struct WordCandidate {
        std::string text;
        uint8_t flag = 0;
    };

    struct WordGroup {
        std::string code;
        std::vector<WordCandidate> candidates;
    };

    struct PredictGroup {
        std::string key;
        std::vector<std::string> candidates;
    };

    bool parse(const uint8_t *blob, size_t size);
    bool valid() const { return _valid; }

    Scheme scheme() const { return _scheme; }
    int codeLen() const { return _codeLen; }
    int recordSize() const { return _recordSize; }
    uint32_t singleCount() const { return _singleCount; }
    uint32_t wordCount() const { return _wordCount; }
    bool hasWords() const { return _wordCount > 0 && _wordData && _wordDataSize > 0; }

    const uint8_t *wordData() const { return _wordData; }
    size_t wordDataSize() const { return _wordDataSize; }
    const uint8_t *predictData() const { return _predictData; }
    size_t predictDataSize() const { return _predictDataSize; }
    uint32_t predictCount() const { return _predictCount; }
    bool hasPredictions() const { return _predictCount > 0 && _predictData && _predictDataSize > 0; }

    void singleWindow(const char *code, int len, uint32_t &lo, uint32_t &hi) const;
    uint32_t lowerBoundSingle(const char *code, int len, uint32_t lo, uint32_t hi) const;
    bool readSingleCode(uint32_t i, char out[kMaxCodeLen + 1]) const;
    bool readSingleText(uint32_t i, char out[kHanziSize + 1]) const;
    uint8_t readSingleFlag(uint32_t i) const;

    void wordWindow(const char *code, int len, size_t &lo, size_t &hi) const;
    bool nextWordGroup(size_t &pos, size_t end, WordGroup &out) const;
    bool nextPredictGroup(size_t &pos, PredictGroup &out) const;
    bool findPredictGroup(const std::string &key, PredictGroup &out) const;

    // 词表桶内的组偏移检查点。桶内组记录变长且没有长度字段, 桶内定位只能从桶首逐组走
    // 过去——每个组还要跨过它自己的每一条词, 代价正比于桶的字节数(实测最坏的 sh 桶
    // 113KB / 3451 组, 整走一遍约 5ms)。整句相位每个 pos 都要定位一次, 长全拼串每敲一键
    // 就重扫一遍, 所以按桶惰性记下每 kWordGroupStride 组的偏移, 之后二分到目标附近,
    // 只线性走至多 Stride 组。
    static const int kWordGroupStride = 16;
    // 桶内第一个码 >= target(strcmp 序)的组偏移; 没有则返回 hi。groupsRead 累加实际检查
    // 的组数(含二分), 调用方用它记账。
    size_t wordGroupSeek(size_t lo, size_t hi, const char *target, int targetLen,
                         int &groupsRead);
    // off 处那个组的码(不含长度字节); off 越界返回 nullptr。
    const uint8_t *wordGroupCode(size_t off, int &len) const;

private:
    struct PredictIndexEntry {
        uint32_t hash = 0;
        uint32_t offset = 0;
    };

    struct WordBucketIndex {
        size_t lo = 0;
        std::vector<uint32_t> ckpt;
    };

    static uint32_t readU32(const uint8_t *p);
    static uint32_t hashBytes(const char *data, size_t len);
    static int utf8CharLen(uint8_t c);
    static bool isPadding(const uint8_t *p, size_t size);
    bool buildPredictIndex() const;
    bool readPredictGroupAt(size_t pos, PredictGroup &out) const;
    // 跳过 off 处的那一组, 返回下一组的偏移(数据坏则返回 end)。推进方式必须和
    // IME::collectSentenceArcs 走桶时一致, 否则检查点会指到组的中间。
    size_t wordGroupAt(size_t off, size_t end) const;
    const WordBucketIndex *wordBucketIndex(size_t lo, size_t hi);

    bool _valid = false;
    const uint8_t *_blob = nullptr;
    size_t _blobSize = 0;

    Scheme _scheme = WUBI;
    int _codeLen = 0;
    int _recordSize = 0;
    uint32_t _singleCount = 0;
    size_t _singleBase = 0;
    std::vector<uint32_t> _singleIndex;

    uint32_t _wordCount = 0;
    std::vector<uint32_t> _wordIndex;
    const uint8_t *_wordData = nullptr;
    size_t _wordDataSize = 0;
    // 组偏移检查点, 按 lo 唯一标识一个桶; 惰性建立, 条数上限是词表里有数据的桶数(152)。
    std::vector<WordBucketIndex> _wordBucketIdx;

    uint32_t _predictCount = 0;
    const uint8_t *_predictData = nullptr;
    size_t _predictDataSize = 0;
    mutable bool _predictIndexBuilt = false;
    mutable std::vector<PredictIndexEntry> _predictIndex;
};

} // namespace ime
