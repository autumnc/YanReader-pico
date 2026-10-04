#!/usr/bin/env python3
"""把万象拼音的词库并进 pjournal 的拼音词表，产出 ime_src/pinyin-utf.txt。

产物格式不变（一行一个码，码后空格分隔候选），直接喂给 generate_ime_bin.py：
单字（3 字节候选）由生成器自动归到 single 段，多字词归到 word 段。

用法：
    python3 scripts/build_ime_source.py            # 从 ime_src/pinyin-base.txt 生成
    python3 scripts/build_ime_source.py --budget 2000000

外部数据（不在仓库里，需自行下载）：
    https://raw.githubusercontent.com/amzxyz/rime-wanxiang/wanxiang/dicts/jichu.dict.yaml
    https://raw.githubusercontent.com/amzxyz/rime-wanxiang/wanxiang/dicts/zi.dict.yaml
    默认按 /tmp/jichu.dict.yaml /tmp/zi.dict.yaml 找，可用 --jichu/--zi 指定。

合并规则（都基于实测，见文件末尾注释）：
  * 单字：万象 zi.dict.yaml 为底 —— 它覆盖全部 6775 个可渲染汉字、且带真实字频；
          再并上现有表里有、万象没有的“另读”条目。
  * 词：现有表**全部保留**，再接万象 jichu.dict.yaml 里权重 >= 阈值的词。
  * **词**：每个码里钉住现有表的第一个候选（现有首选永不被换掉），其余（现有候选 +
    万象新词）按万象权重降序排，权重相同或万象没有的保持原顺序。
  * **单字**：不按权重排，现有候选原样在前、万象新增的同音字按权重降序接在后面 ——
    zi.dict 的同音字频次噪声比词库大得多（"shi" 里 实 排不进前八、被 拾/式/视/饰 反超），
    照它排会把常用字挤下去。
    为什么不直接全按权重重排：万象词频有明显语料噪声（老是 409258 压 老师 10767、
    打啥 压 大厦），全按它排会在 12.7% 的码上换掉我们的首选、且抽样看多数换得更差；
    改成"钉首选 + 其余按权重"后只动 2.5% 的码，且实测位移都是冷僻自造词（多紧/记时/没信）
    给常用词让位，方向是对的。
  * 字体渲染不出来的单字（builtin.ttf 子集之外）**保留但沉底**（老表里这种单字有
    1756 条，全是繁体/日文汉字/生僻名用字）。换扩展字体后它们就画得出来了，所以
    不该从表里删掉；但用当前字体时它们只会是豆腐块，所以排在所有可渲染候选之后：
        可渲染（原表原序 + 万象按权重）  →  取名常用字（按权重）  →  其余生僻字（按权重）
    取名常用字白名单在 ime_src/name-common.txt，人工维护。
  * 生僻字若连 GB18030 两字节都编不出来（即 GBK 之外），说明连输入法/字体生态都
    很少覆盖，**安全删掉**；被删的那批里只有 㸆 一个落在这一档。
"""

import argparse
import collections
import json
import re
import struct
import sys
import unicodedata

# --- 可渲染字符集：直接从 main/assets/builtin.ttf 的 cmap 表读，不依赖 fontTools ---


def _ttf_cmap(path):
    d = open(path, 'rb').read()
    num_tables = struct.unpack_from('>H', d, 4)[0]
    cmap = None
    for i in range(num_tables):
        off = 12 + 16 * i
        tag = d[off:off + 4]
        if tag == b'cmap':
            cmap = struct.unpack_from('>I', d, off + 8)[0]
            break
    if cmap is None:
        raise SystemExit('builtin.ttf: 没有 cmap 表')
    n = struct.unpack_from('>H', d, cmap + 2)[0]
    sub = None
    for i in range(n):
        off = cmap + 4 + 8 * i
        pid, eid, sub_off = struct.unpack_from('>HHI', d, off)
        fmt = struct.unpack_from('>H', d, cmap + sub_off)[0]
        if fmt in (4, 12):
            sub = cmap + sub_off
    if sub is None:
        raise SystemExit('builtin.ttf: cmap 里没有 format 4/12 子表')
    cps = set()
    fmt = struct.unpack_from('>H', d, sub)[0]
    if fmt == 12:
        ngroups = struct.unpack_from('>I', d, sub + 12)[0]
        for i in range(ngroups):
            s, e, _ = struct.unpack_from('>III', d, sub + 16 + 12 * i)
            cps.update(range(s, e + 1))
    else:
        segx2 = struct.unpack_from('>H', d, sub + 6)[0]
        seg = segx2 // 2
        base = sub + 14
        ends = struct.unpack_from('>%dH' % seg, d, base)
        starts = struct.unpack_from('>%dH' % seg, d, base + segx2 + 2)
        deltas = struct.unpack_from('>%dh' % seg, d, base + 2 * (segx2 + 2))
        range_off_pos = base + 3 * (segx2 + 2)
        for i in range(seg):
            if starts[i] > ends[i]:
                continue
            ro = struct.unpack_from('>H', d, range_off_pos + 2 * i)[0]
            for c in range(starts[i], min(ends[i], 0xFFFF) + 1):
                # format 4 的老规矩：**按 idRangeOffset 是否为 0 分岔**，不是按 idDelta。
                # 分岔条件写错的话，Noto 里一大批 ASCII 标点（空格/破折号/引号）会被漏掉。
                if ro != 0:
                    gid = struct.unpack_from(
                        '>H', d, range_off_pos + 2 * i + ro + 2 * (c - starts[i]))[0]
                    if gid == 0:
                        continue
                    gid = (gid + deltas[i]) & 0xFFFF
                else:
                    gid = (c + deltas[i]) & 0xFFFF
                if gid != 0:
                    cps.add(c)
    return cps


def load_renderable(ttf):
    # 判据就是"字体画不画得出来"，所以取 cmap 全部码位（CJK 6775 + 拉丁/标点等 942），
    # **不要**只留 CJK —— 词表里有 "AA制"、"3D" 这类夹非汉字的词，那些字是有字形的。
    return set(chr(c) for c in _ttf_cmap(ttf))


def load_name_common(path):
    """取名常用字白名单：'#' 起注释，其余按空白切字。文件不存在就当空集。"""
    try:
        text = open(path, encoding='utf-8').read()
    except OSError:
        return set()
    # 一行里连着写多个字也要认：逐字符取、跳过空白。
    return set(ch for line in text.splitlines() if not line.lstrip().startswith('#')
               for ch in line if not ch.isspace())


def gb18030_wide(ch):
    """True = 连 GB18030 两字节都编不出来（GBK 之外），可以安全丢弃。"""
    try:
        return len(ch.encode('gb18030')) > 2
    except UnicodeEncodeError:
        return True


# --- 拼音码归一化 ---


def conv(code):
    """带调拼音 → 小写 ASCII 码。ü(0x308) → v（本仓惯例 lv/nv/lve/nve）。"""
    out = []
    for ch in unicodedata.normalize('NFD', code):
        o = ord(ch)
        if o == 0x308:
            out.append('v')
        elif 0x300 <= o <= 0x36F:
            continue
        else:
            out.append(ch)
    return ''.join(out).lower().replace(' ', '')


def clean_code(raw):
    return re.sub(r'[^a-z]', '', re.sub(r'^\{[0-9]+\}', '', raw).lower())


def parse_ours(path):
    """返回 (单字表, 词表)，都是 code -> [候选]。顺序 = 文件顺序。

    **不按可渲染性过滤** —— 单字段的沉底/丢弃交给 emit 阶段判（见 main），
    这样"原表本来收了这个字"这件事得以保留下来。
    """
    singles, words = collections.defaultdict(list), collections.defaultdict(list)
    for line in open(path, encoding='utf-8-sig'):
        line = line.strip()
        if not line or line.startswith('#'):
            continue
        p = line.split()
        if len(p) < 2:
            continue
        code = clean_code(p[0])
        if not code:
            continue
        for c in p[1:]:
            (singles if len(c.encode('utf-8')) == 3 else words)[code].append(c)
    return singles, words


def parse_wx(path, render, maxcode):
    """万象词典 → {(code, 候选): 权重}，只保留全部字可渲染、码长合法的条目。"""
    best = {}
    for line in open(path, encoding='utf-8'):
        if line.startswith('#') or not line.strip():
            continue
        p = line.rstrip('\n').split('\t')
        if len(p) < 3:
            continue
        try:
            wt = int(p[2])
        except ValueError:
            continue
        w = p[0].strip()
        if not w or any(ch not in render for ch in w):
            continue
        code = conv(p[1])
        if not code or len(code) > maxcode or not re.fullmatch(r'[a-z]+', code):
            continue
        k = (code, w)
        if wt > best.get(k, -1):
            best[k] = wt
    return best


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--base', default='ime_src/pinyin-base.txt')
    ap.add_argument('--out', default='ime_src/pinyin-utf.txt')
    ap.add_argument('--jichu', default='/tmp/jichu.dict.yaml')
    ap.add_argument('--zi', default='/tmp/zi.dict.yaml')
    ap.add_argument('--ttf', default='main/assets/builtin.ttf')
    ap.add_argument('--name-common', default='ime_src/name-common.txt',
                    help='取名常用字白名单（这些字不按生僻字沉底）')
    ap.add_argument('--thresh', type=int, default=800, help='万象词的最低权重')
    ap.add_argument('--budget', type=int, default=2_600_000,
                    help='万象词段的估算字节上限（词体 + 摊派组头）')
    ap.add_argument('--max-per-code', type=int, default=128)
    args = ap.parse_args()

    render = load_renderable(args.ttf)
    namec = load_name_common(args.name_common)
    ours_s, ours_w = parse_ours(args.base)
    wx_s = parse_wx(args.zi, render, 6)          # 单字码受 kMaxCodeLen=6 限制
    wx_w = parse_wx(args.jichu, render, 24)

    # 单字里的多字条目不算单字；词里的单字条目交给 zi 段
    wx_s = {k: v for k, v in wx_s.items() if len(k[1]) == 1}
    wx_w = {k: v for k, v in wx_w.items()
            if not (len(k[1]) == 1 and len(k[1].encode()) == 3)}

    # 按预算裁万象词：权重降序（同权按码/词定序，保证可复现）
    ranked = sorted(((v, k[0], k[1]) for k, v in wx_w.items()),
                    key=lambda t: (-t[0], t[1], t[2]))
    kept, approx = [], 0
    for wt, code, w in ranked:
        if wt < args.thresh:
            break
        approx += 2 + len(w.encode('utf-8')) + 9    # 词体 + 摊派组头
        if approx > args.budget:
            break
        kept.append((code, w, wt))
    print(f'万象词入选 {len(kept)} 条（阈值 {args.thresh}，估算 {approx / 1e6:.2f}MB）',
          file=sys.stderr)

    # code -> [[权重, 序号, 候选, 是否本仓原表, 档位]]；档位见 emit_singles
    #   0 = 原表有 + 字体画得出      1 = 万象补的 + 字体画得出
    #   2 = 取名常用字（画不出）     3 = 生僻字（画不出）
    singles, words = collections.defaultdict(list), collections.defaultdict(list)
    dropped_gbk, dropped_word = [], 0
    order = 0
    for code, cs in ours_s.items():
        for c in cs:
            if c in render:
                tier = 0
            elif gb18030_wide(c):
                dropped_gbk.append(c)      # GBK 都编不出来 → 安全丢弃
                continue
            else:
                tier = 2 if c in namec else 3
            singles[code].append([wx_s.get((code, c), 0), order, c, True, tier]); order += 1
    for code, cs in ours_w.items():
        for c in cs:
            if any(ch not in render for ch in c):
                dropped_word += 1          # 词里的画不出的字：整条丢掉（词不能半截豆腐块）
                continue
            words[code].append([wx_w.get((code, c), 0), order, c, True, 0]); order += 1
    for (code, ch), wt in wx_s.items():
        singles[code].append([wt, order, ch, False, 1]); order += 1
    for code, w, wt in kept:
        words[code].append([wt, order, w, False, 0]); order += 1
    print(f'单字沉底：取名常用字 {sum(1 for v in singles.values() for it in v if it[4] == 2)} 条、'
          f'生僻 {sum(1 for v in singles.values() for it in v if it[4] == 3)} 条；'
          f'超 GBK 丢弃 {len(dropped_gbk)} 条、含生僻字的词丢弃 {dropped_word} 条',
          file=sys.stderr)

    def emit(d, reweight):
        for code in sorted(d):
            items = d[code]
            if reweight:
                # 钉住现有表的第一个（现有首选），其余按权重降序、同分保持原顺序
                pinned = next((it for it in items if it[3]), None)
                rest = [it for it in items if it is not pinned]
                rest.sort(key=lambda t: (-t[0], t[1]))
                items = ([pinned] if pinned else []) + rest
            else:
                # 先按档位分段（0/1 可渲染 → 2 取名常用字 → 3 生僻）；
                # 0/1 段内维持老规矩（原表原序在前、万象补的按权重降序接后面），
                # 2/3 段内一律按语料权重降序 —— 权重取自万象单字表，原表旧条目
                # 若万象里没有，权重为 0，自然落到该段最末。
                def key(t):
                    tier = t[4]
                    if tier == 0:
                        return (0, 0, t[1])
                    if tier == 1:
                        return (1, -t[0], t[1])
                    return (tier, -t[0], t[1])
                items = sorted(items, key=key)
            seen, cands = set(), []
            for it in items:
                c = it[2]
                if c in seen:
                    continue
                seen.add(c)
                cands.append(c)
            yield code, cands

    lines = []
    for code, cands in emit(singles, reweight=False):
        lines.append(code + ' ' + ' '.join(cands[:args.max_per_code]))
    for code, cands in emit(words, reweight=True):
        lines.append(code + ' ' + ' '.join(cands[:args.max_per_code]))
    with open(args.out, 'w', encoding='utf-8') as f:
        f.write('\n'.join(lines) + '\n')
    print(f'写出 {args.out}：{len(lines)} 行 / '
          f'{sum(len(l.split()) - 1 for l in lines)} 候选 / '
          f'{sum(len(l) + 1 for l in lines) / 1e6:.2f}MB 文本')


if __name__ == '__main__':
    main()
