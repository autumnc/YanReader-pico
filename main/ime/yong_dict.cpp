#include "yong_dict.h"

#include <algorithm>
#include <cstring>
#include <utility>

namespace ime {

static const uint8_t kIm3Magic[4] = {'I', 'M', 'E', '3'};

uint32_t Im3Dictionary::readU32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

uint32_t Im3Dictionary::hashBytes(const char *data, size_t len) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; i++) {
        h ^= (uint8_t)data[i];
        h *= 16777619u;
    }
    return h ? h : 1;
}

int Im3Dictionary::utf8CharLen(uint8_t c) {
    if ((c & 0x80) == 0) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 0;
}

bool Im3Dictionary::isPadding(const uint8_t *p, size_t size) {
    for (size_t i = 0; i < size; i++) {
        if (p[i] != 0x00 && p[i] != 0xff) return false;
    }
    return true;
}

bool Im3Dictionary::parse(const uint8_t *blob, size_t size) {
    _valid = false;
    _blob = nullptr;
    _blobSize = 0;
    _scheme = WUBI;
    _codeLen = 0;
    _recordSize = 0;
    _singleCount = 0;
    _singleBase = 0;
    _singleIndex.clear();
    _wordCount = 0;
    _wordIndex.clear();
    _wordData = nullptr;
    _wordDataSize = 0;
    _wordBucketIdx.clear();
    _predictCount = 0;
    _predictData = nullptr;
    _predictDataSize = 0;
    _predictIndexBuilt = false;
    _predictIndex.clear();

    if (!blob || size < kHeaderSize || std::memcmp(blob, kIm3Magic, sizeof(kIm3Magic)) != 0)
        return false;

    int codeLen = blob[5];
    if (codeLen < 1 || codeLen > kMaxCodeLen) return false;

    uint32_t singleCount = readU32(blob + 8);
    size_t singleBase = kHeaderSize + (size_t)kIndexEntries * 4;
    size_t recordSize = (size_t)codeLen + kHanziSize + kFlagSize;
    size_t singleEnd = singleBase + (size_t)singleCount * recordSize;
    if (singleCount == 0 || singleEnd > size) return false;

    _blob = blob;
    _blobSize = size;
    _scheme = (Scheme)blob[4];
    _codeLen = codeLen;
    _recordSize = (int)recordSize;
    _singleCount = singleCount;
    _singleBase = singleBase;
    _singleIndex.resize(kIndexEntries);
    for (int i = 0; i < kIndexEntries; i++)
        _singleIndex[i] = readU32(blob + kHeaderSize + (size_t)i * 4);

    size_t wordBase = singleEnd;
    if (wordBase + 4 <= size) {
        const uint8_t *wp = blob + wordBase;
        _wordCount = readU32(wp);
        if (_wordCount > 0 && wordBase + 4 + (size_t)kIndexEntries * 4 <= size) {
            wp += 4;
            _wordIndex.resize(kIndexEntries);
            for (int i = 0; i < kIndexEntries; i++)
                _wordIndex[i] = readU32(wp + (size_t)i * 4);
            wp += (size_t)kIndexEntries * 4;
            size_t maxWordDataSize = size - singleEnd - 4 - (size_t)kIndexEntries * 4;
            size_t wordDataSize = _wordIndex[kIndexEntries - 1];
            bool indexOk = wordDataSize <= maxWordDataSize;
            for (int i = 1; indexOk && i < kIndexEntries; i++) {
                if (_wordIndex[i] < _wordIndex[i - 1] || _wordIndex[i] > wordDataSize)
                    indexOk = false;
            }
            if (indexOk) {
                _wordData = wp;
                _wordDataSize = wordDataSize;
            } else {
                _wordCount = 0;
                _wordIndex.clear();
            }

            size_t predBase = wordBase + 4 + (size_t)kIndexEntries * 4 + _wordDataSize;
            if (_wordData && predBase + 4 <= size && !isPadding(blob + predBase, size - predBase)) {
                const uint8_t *pp = blob + predBase;
                uint32_t predictCount = readU32(pp);
                if (predictCount > 0 && predictCount < 100000) {
                    _predictCount = predictCount;
                    _predictData = pp + 4;
                    _predictDataSize = size - predBase - 4;
                }
            }
        }
    }

    _valid = true;
    return true;
}

void Im3Dictionary::singleWindow(const char *code, int len, uint32_t &lo, uint32_t &hi) const {
    lo = 0;
    hi = _singleCount;
    if (!_valid || _singleIndex.empty() || !code || len < 1) return;
    int c0 = code[0] - 'a';
    if (c0 < 0 || c0 >= 26) return;
    if (len == 1) {
        lo = _singleIndex[c0 * 26];
        hi = _singleIndex[(c0 + 1) * 26];
        return;
    }
    int c1 = code[1] - 'a';
    if (c1 < 0 || c1 >= 26) return;
    int k = c0 * 26 + c1;
    lo = _singleIndex[k];
    hi = _singleIndex[k + 1];
}

uint32_t Im3Dictionary::lowerBoundSingle(const char *code, int len, uint32_t lo, uint32_t hi) const {
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        char item[kMaxCodeLen + 1];
        if (!readSingleCode(mid, item)) break;
        if (std::strncmp(item, code, len) < 0) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

bool Im3Dictionary::readSingleCode(uint32_t i, char out[kMaxCodeLen + 1]) const {
    if (!_valid || !out || i >= _singleCount) return false;
    const uint8_t *rec = _blob + _singleBase + (size_t)i * _recordSize;
    int n = 0;
    for (; n < _codeLen && rec[n]; n++) out[n] = (char)rec[n];
    out[n] = '\0';
    return true;
}

bool Im3Dictionary::readSingleText(uint32_t i, char out[kHanziSize + 1]) const {
    if (!_valid || !out || i >= _singleCount) return false;
    const uint8_t *rec = _blob + _singleBase + (size_t)i * _recordSize + _codeLen;
    out[0] = (char)rec[0];
    out[1] = (char)rec[1];
    out[2] = (char)rec[2];
    out[3] = '\0';
    return true;
}

uint8_t Im3Dictionary::readSingleFlag(uint32_t i) const {
    if (!_valid || i >= _singleCount) return 0;
    const uint8_t *rec = _blob + _singleBase + (size_t)i * _recordSize;
    return rec[_codeLen + kHanziSize];
}

void Im3Dictionary::wordWindow(const char *code, int len, size_t &lo, size_t &hi) const {
    lo = 0;
    hi = _wordDataSize;
    if (!_valid || _wordIndex.empty() || !_wordData || !code || len < 1) return;
    int c0 = code[0] - 'a';
    if (c0 < 0 || c0 >= 26) return;
    if (len == 1) {
        lo = _wordIndex[c0 * 26];
        hi = (c0 + 1 < 26) ? _wordIndex[(c0 + 1) * 26] : _wordIndex[kIndexEntries - 1];
        return;
    }
    int c1 = code[1] - 'a';
    if (c1 < 0 || c1 >= 26) return;
    int k = c0 * 26 + c1;
    lo = _wordIndex[k];
    hi = (k + 1 < kIndexEntries) ? _wordIndex[k + 1] : _wordDataSize;
}

bool Im3Dictionary::nextWordGroup(size_t &pos, size_t end, WordGroup &out) const {
    out.code.clear();
    out.candidates.clear();
    if (!_valid || !_wordData || pos >= end || end > _wordDataSize) return false;

    uint8_t cl = _wordData[pos];
    if (cl == 0 || pos + 1 + cl > end) {
        pos = end;
        return false;
    }
    out.code.assign((const char *)_wordData + pos + 1, cl);
    pos += 1 + cl;
    if (pos >= end) {
        pos = end;
        return false;
    }

    uint8_t n = _wordData[pos++];
    out.candidates.reserve(n);
    for (uint8_t i = 0; i < n && pos < end; i++) {
        uint8_t wl = _wordData[pos++];
        if (wl == 0 || pos + wl + 1 > end) {
            pos = end;
            return false;
        }
        WordCandidate cand;
        cand.text.assign((const char *)_wordData + pos, wl);
        cand.flag = _wordData[pos + wl];
        out.candidates.push_back(std::move(cand));
        pos += wl + 1;
    }
    return true;
}

// 按字节字典序比较组的码与目标串, 与桶内排序(strcmp)一致: 长度不等而短者是前缀时短者小。
static int wordGroupCompare(const uint8_t *code, int codeLen,
                            const uint8_t *target, int targetLen) {
    int n = codeLen < targetLen ? codeLen : targetLen;
    int c = n > 0 ? std::memcmp(code, target, (size_t)n) : 0;
    return c != 0 ? c : (codeLen - targetLen);
}

// 跳过 off 处的那一组, 返回下一组偏移; 解析坏则返回 end。推进方式必须与
// IME::collectSentenceArcs 走桶时完全一致, 否则检查点会指到组的中间。
size_t Im3Dictionary::wordGroupAt(size_t off, size_t end) const {
    if (!_wordData || off >= end || end > _wordDataSize) return end;
    uint8_t cl = _wordData[off];
    if (cl == 0 || off + 1 + cl > end) return end;
    size_t p = off + 1 + cl;
    if (p >= end) return end;
    uint8_t n = _wordData[p++];
    for (uint8_t j = 0; j < n; j++) {
        if (p + 1 > end) return end;
        uint8_t wl = _wordData[p];
        if (wl == 0 || p + 1 + wl + 1 > end) return end;
        p += 1 + wl + 1;
    }
    return p;
}

const uint8_t *Im3Dictionary::wordGroupCode(size_t off, int &len) const {
    len = 0;
    if (!_wordData || off >= _wordDataSize) return nullptr;
    int cl = _wordData[off];
    if (cl == 0) return nullptr;
    len = cl;
    return _wordData + off + 1;
}

// 桶内每 kWordGroupStride 组记一个偏移检查点, 惰性建立、按 lo 缓存(桶由 lo 唯一标识)。
const Im3Dictionary::WordBucketIndex *Im3Dictionary::wordBucketIndex(size_t lo, size_t hi) {
    if (!_wordData || lo >= hi || hi > _wordDataSize) return nullptr;
    for (const WordBucketIndex &bucket : _wordBucketIdx) {
        if (bucket.lo == lo) return &bucket;
    }
    WordBucketIndex bucket;
    bucket.lo = lo;
    size_t p = lo;
    int since = 0;
    while (p < hi) {
        if (since == 0) bucket.ckpt.push_back((uint32_t)p);
        size_t next = wordGroupAt(p, hi);
        if (next <= p || next > hi) break;
        p = next;
        if (++since >= kWordGroupStride) since = 0;
    }
    if (bucket.ckpt.empty()) return nullptr;
    _wordBucketIdx.push_back(std::move(bucket));
    return &_wordBucketIdx.back();
}

size_t Im3Dictionary::wordGroupSeek(size_t lo, size_t hi, const char *target, int targetLen,
                                    int &groupsRead) {
    const WordBucketIndex *bucket = wordBucketIndex(lo, hi);
    if (!bucket || bucket->ckpt.empty()) return hi;
    const uint8_t *tb = (const uint8_t *)target;
    const std::vector<uint32_t> &ck = bucket->ckpt;
    // 二分到第一个 >= target 的检查点(检查点本身也是一个组)。
    size_t a = 0, b = ck.size();
    while (a < b) {
        size_t mid = a + (b - a) / 2;
        int cl = 0;
        const uint8_t *code = wordGroupCode(ck[mid], cl);
        if (!code || wordGroupCompare(code, cl, tb, targetLen) < 0) a = mid + 1;
        else b = mid;
    }
    size_t off = ck[a > 0 ? a - 1 : 0];
    while (off < hi) {
        int cl = 0;
        const uint8_t *code = wordGroupCode(off, cl);
        if (!code) break;
        groupsRead++;
        if (wordGroupCompare(code, cl, tb, targetLen) >= 0) return off;
        size_t next = wordGroupAt(off, hi);
        if (next <= off) break;
        off = next;
    }
    return hi;
}

bool Im3Dictionary::nextPredictGroup(size_t &pos, PredictGroup &out) const {
    size_t start = pos;
    if (!readPredictGroupAt(start, out)) {
        pos = _predictDataSize;
        return false;
    }
    pos = start;
    int keyLen = utf8CharLen(_predictData[pos]);
    pos += (size_t)keyLen + 1;
    for (uint8_t i = 0, n = (uint8_t)out.candidates.size(); i < n && pos < _predictDataSize; i++) {
        uint8_t wl = _predictData[pos++];
        pos += wl;
    }
    return true;
}

bool Im3Dictionary::readPredictGroupAt(size_t pos, PredictGroup &out) const {
    out.key.clear();
    out.candidates.clear();
    if (!_valid || !_predictData || pos >= _predictDataSize) return false;

    const uint8_t *base = _predictData;
    size_t end = _predictDataSize;
    int keyLen = utf8CharLen(base[pos]);
    if (keyLen <= 0 || pos + (size_t)keyLen + 1 > end) {
        pos = end;
        return false;
    }

    out.key.assign((const char *)base + pos, keyLen);
    pos += keyLen;
    uint8_t n = base[pos++];
    out.candidates.reserve(n);
    for (uint8_t i = 0; i < n && pos < end; i++) {
        uint8_t wl = base[pos++];
        if (wl == 0 || pos + wl > end) {
            pos = end;
            return false;
        }
        out.candidates.emplace_back((const char *)base + pos, wl);
        pos += wl;
    }
    return true;
}

bool Im3Dictionary::buildPredictIndex() const {
    if (_predictIndexBuilt) return !_predictIndex.empty();
    _predictIndexBuilt = true;
    _predictIndex.clear();
    if (!_valid || !_predictData || _predictCount == 0) return false;
    if (_predictCount > PJOURNAL_IME_PREDICT_INDEX_MAX_GROUPS) return false;

    size_t pos = 0;
    for (uint32_t group = 0; group < _predictCount && pos < _predictDataSize; group++) {
        size_t start = pos;
        int keyLen = utf8CharLen(_predictData[pos]);
        if (keyLen <= 0 || pos + (size_t)keyLen + 1 > _predictDataSize) {
            _predictIndex.clear();
            return false;
        }
        uint32_t h = hashBytes((const char *)_predictData + pos, (size_t)keyLen);
        pos += (size_t)keyLen;
        uint8_t n = _predictData[pos++];
        bool ok = true;
        for (uint8_t i = 0; i < n && ok; i++) {
            if (pos >= _predictDataSize) { ok = false; break; }
            uint8_t wl = _predictData[pos++];
            if (wl == 0 || pos + wl > _predictDataSize) ok = false;
            else pos += wl;
        }
        if (!ok || start > UINT32_MAX) {
            _predictIndex.clear();
            return false;
        }
        _predictIndex.push_back({h, (uint32_t)start});
    }
    std::stable_sort(_predictIndex.begin(), _predictIndex.end(),
        [](const PredictIndexEntry &a, const PredictIndexEntry &b) {
            if (a.hash != b.hash) return a.hash < b.hash;
            return a.offset < b.offset;
        });
    return !_predictIndex.empty();
}

bool Im3Dictionary::findPredictGroup(const std::string &key, PredictGroup &out) const {
    out.key.clear();
    out.candidates.clear();
    if (!_valid || !_predictData || key.empty()) return false;

    if (buildPredictIndex()) {
        uint32_t h = hashBytes(key.data(), key.size());
        auto lo = std::lower_bound(_predictIndex.begin(), _predictIndex.end(), h,
            [](const PredictIndexEntry &entry, uint32_t value) { return entry.hash < value; });
        auto hi = std::upper_bound(lo, _predictIndex.end(), h,
            [](uint32_t value, const PredictIndexEntry &entry) { return value < entry.hash; });
        for (auto it = lo; it != hi; ++it) {
            if (readPredictGroupAt(it->offset, out) && out.key == key) return true;
        }
        out.key.clear();
        out.candidates.clear();
        return false;
    }

    const uint8_t *base = _predictData;
    size_t pos = 0;
    size_t end = _predictDataSize;
    while (pos < end) {
        int keyLen = utf8CharLen(base[pos]);
        if (keyLen <= 0 || pos + (size_t)keyLen + 1 > end) return false;
        bool matched = key.size() == (size_t)keyLen &&
                       std::memcmp(base + pos, key.data(), keyLen) == 0;
        size_t keyPos = pos;
        pos += keyLen;
        uint8_t n = base[pos++];
        if (matched) {
            out.key.assign((const char *)base + keyPos, keyLen);
            out.candidates.reserve(n);
        }
        for (uint8_t i = 0; i < n && pos < end; i++) {
            uint8_t wl = base[pos++];
            if (wl == 0 || pos + wl > end) return false;
            if (matched) out.candidates.emplace_back((const char *)base + pos, wl);
            pos += wl;
        }
        if (matched) return true;
    }
    return false;
}

} // namespace ime
