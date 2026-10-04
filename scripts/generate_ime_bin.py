#!/usr/bin/env python3
"""Generate ime_table_pinyin.bin (IME3 format) from source dictionaries.

Sources (external, not in the repo):
  pinyin-utf.txt   pinyin code + space-separated candidates (single chars and
                   multi-char words mixed).  `{N}code` marks a cross-syllable
                   boundary word with weight N; the weight is discarded here.
  predict_source.txt  key<TAB>candidate... (next-word prediction / 联想).

Layout produced (matches Im3Dictionary::parse in main/ime/yong_dict.cpp):
  [12]      header: "IME3" + scheme(1=PINYIN) + code_len(6) + 2 pad + single_count u32
  [677*4]   single index: idx[k] = # single records with rank < key(k)
  [N*10]    single records: code(6, NUL-padded) + hanzi(3, UTF-8) + flag(0)
  [4]       word_count u32 (number of word groups)
  [677*4]   word index: byte offset of each bucket, sentinel = word data size
  [..]      word data: buckets 0..675, each group [cl][code][n][n*(wl,word,flag)]
  [4]       predict_count u32
  [..]      predict data: [key UTF-8][n][n*(wl,word)] per group

rank(code): single-char codes map to their doubled prefix (e.g. "a" -> "aa") so
that the 1-letter syllable shares the first bucket with the two-letter codes.
"""

import re
import struct
import sys
from collections import Counter, defaultdict

HEADER = 12
INDEX_ENTRIES = 26 * 26 + 1  # 677
CODELEN = 6
REC = CODELEN + 3 + 1  # 10


def rank(code):
    if len(code) < 2:
        return code + "a"
    return code[:2]


def build_index(codes):
    """idx[k] = count of codes whose rank sorts before the 2-char key(k)."""
    idx = [0] * INDEX_ENTRIES
    counts = Counter(rank(c) for c in codes)
    tot = 0
    for c0 in range(26):
        for c1 in range(26):
            k = c0 * 26 + c1
            idx[k] = tot
            tot += counts.get(chr(97 + c0) + chr(97 + c1), 0)
    idx[INDEX_ENTRIES - 1] = tot
    return idx


def clean_code(raw):
    m = re.match(r"^\{(\d+)\}(.*)$", raw)
    if m:
        raw = m.group(2)
    return re.sub(r"[^a-z]", "", raw.lower())


def parse_pinyin(path):
    singles = []            # (code, char)
    words = defaultdict(list)  # code -> [word, ...]
    for line in open(path, encoding="utf-8-sig"):
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        parts = line.split()
        if len(parts) < 2:
            continue
        code = clean_code(parts[0])
        if not code:
            continue
        for cand in parts[1:]:
            if len(cand.encode("utf-8")) == 3:
                singles.append((code, cand))
            else:
                words[code].append(cand)
    return singles, words


def parse_predict(path):
    # The runtime reader (Im3Dictionary::readPredictGroupAt) stores the key as a
    # single UTF-8 char (utf8CharLen of the first byte), so multi-char keys in
    # predict_source.txt can never be matched and would corrupt the linear scan.
    # Only single-char keys are usable; 2-4 char tails are served by the
    # compiled-in BUILTIN_PREDICT table and the user dictionary instead.
    groups = {}
    order = []
    for line in open(path, encoding="utf-8"):
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        if "\t" in line:
            key, rest = line.split("\t", 1)
            ws = rest.replace("\t", " ").split()
        else:
            parts = line.split()
            if len(parts) < 2:
                continue
            key, ws = parts[0], parts[1:]
        key = key.strip()
        if len(key) != 1:
            continue
        if key not in groups:
            groups[key] = []
            order.append(key)
        for w in ws:
            if w and w not in groups[key]:
                groups[key].append(w)
        groups[key] = groups[key][:16]
    return [(key, groups[key]) for key in order if groups[key]]


def build(pinyin_path, predict_path, out_path):
    singles, words = parse_pinyin(pinyin_path)

    # dedup single (code, char), then stable sort by (rank, code)
    seen = set()
    uniq = []
    for code, ch in singles:
        k = (code, ch)
        if k not in seen:
            seen.add(k)
            uniq.append((code, ch))
    uniq.sort(key=lambda r: (rank(r[0]), r[0]))

    single_count = len(uniq)
    hdr = b"IME3" + bytes([1, CODELEN, 0, 0]) + struct.pack("<I", single_count)
    idx_bytes = b"".join(struct.pack("<I", v) for v in build_index([r[0] for r in uniq]))
    rec_bytes = b"".join(
        code.encode().ljust(CODELEN, b"\x00") + ch.encode("utf-8") + b"\x00"
        for code, ch in uniq
    )

    # word groups: dedup per code, sort by (rank, code), then lay out buckets
    word_groups = []
    for code in sorted(words, key=lambda c: (rank(c), c)):
        ws = []
        for w in words[code]:
            if w not in ws:
                ws.append(w)
        if ws:
            word_groups.append((code, ws))
    word_count = len(word_groups)

    def bucket_of(code):
        c0 = ord(code[0]) - 97
        c1 = ord(code[1]) - 97 if len(code) >= 2 else c0
        return c0 * 26 + c1

    buckets = [[] for _ in range(INDEX_ENTRIES - 1)]
    for code, ws in word_groups:
        b = bucket_of(code)
        if 0 <= b < INDEX_ENTRIES - 1:
            buckets[b].append((code, ws))

    word_data = bytearray()
    word_index = [0] * INDEX_ENTRIES
    for b in range(INDEX_ENTRIES - 1):
        word_index[b] = len(word_data)
        for code, ws in buckets[b]:
            cb = code.encode("ascii")
            word_data.append(len(cb))
            word_data.extend(cb)
            word_data.append(len(ws))
            for w in ws:
                wb = w.encode("utf-8")
                word_data.append(len(wb))
                word_data.extend(wb)
                word_data.append(0)  # flag
    word_index[INDEX_ENTRIES - 1] = len(word_data)

    # predict section (last, no trailing padding: parse() reads to EOF)
    pred_groups = parse_predict(predict_path)
    pred = bytearray(struct.pack("<I", len(pred_groups)))
    for key, ws in pred_groups:
        pred.extend(key.encode("utf-8"))
        pred.append(len(ws))
        for w in ws:
            wb = w.encode("utf-8")
            pred.append(len(wb))
            pred.extend(wb)

    out = bytearray()
    out += hdr
    out += idx_bytes
    out += rec_bytes
    out += struct.pack("<I", word_count)
    out += b"".join(struct.pack("<I", v) for v in word_index)
    out += word_data
    out += pred

    with open(out_path, "wb") as f:
        f.write(out)

    print(f"single_count={single_count}")
    print(f"word_groups={word_count}  word_data_size={word_index[-1]}")
    print(f"predict_groups={len(pred_groups)}")
    print(f"total={len(out)} bytes -> {out_path}")


if __name__ == "__main__":
    args = sys.argv[1:]
    pinyin = args[0] if len(args) > 0 else "ime_src/pinyin-utf.txt"
    predict = args[1] if len(args) > 1 else "main/ime/predict_source.txt"
    out = args[2] if len(args) > 2 else "main/ime/ime_table_pinyin.bin"
    build(pinyin, predict, out)
