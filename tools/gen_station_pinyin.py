#!/usr/bin/env python3
# gen_station_pinyin.py — 为 Stations.h 里的每个电台生成拼音检索串。
#
# 输出 src/StationsPinyin.h，包含两个与电台表同序（按 flat 顺序）的数组：
#   kPinyinFull[]   全拼小写连写，如 "zhongguozhisheng"
#   kPinyinInit[]   拼音首字母连写，如 "zgz"
# 搜索时三种方式任一命中即算匹配：台名子串 / 全拼子串 / 首字母子串。
#
# 必须在改动 Stations.h 后重新运行，保持两表同步：
#   python tools/gen_station_pinyin.py
import re
import sys
from pypinyin import lazy_pinyin, Style

SRC = 'src/Stations.h'
DST = 'src/StationsPinyin.h'

src = open(SRC, encoding='utf-8').read()
pat = re.compile(r'\{\s*"([^"]+)",\s*"([^"]+)",\s*\d+,\s*kCodec\w+\s*\}')
entries = pat.findall(src)
if not entries:
    sys.exit('no station entries found in ' + SRC)

fulls, inits = [], []
for name, _url in entries:
    py = lazy_pinyin(name, style=Style.NORMAL, errors='default')
    # 只保留 ASCII 字母数字并转小写：非 ASCII 字符是 UTF-8 多字节，
    # 会撑爆定长数组，对检索也没有价值。
    keep = lambda s: ''.join(c for c in s.lower() if c.isascii() and c.isalnum())
    full = ''.join(keep(p) for p in py)
    # 首字母：汉字取拼音首字母，字母数字段取段首字符，纯数字段逐字符保留
    parts = []
    for p in py:
        p = p.lower()
        if not p:
            continue
        if re.match(r'[a-z]', p):
            parts.append(p[0])
        else:
            parts.extend(c for c in keep(p))
    ini = ''.join(parts)
    fulls.append(full[:31])
    inits.append(ini[:15])

out = []
out.append('// StationsPinyin.h — 电台拼音检索表（由 tools/gen_station_pinyin.py 生成）')
out.append('// 与 Stations.h 的电台 flat 顺序一一对应；改电台表后必须重新生成。')
out.append('#pragma once')
out.append('')
out.append('#include <pgmspace.h>')
out.append('')
out.append('#define STATIONS_PINYIN_COUNT %d' % len(entries))
out.append('')
out.append('static const char kPinyinFull[STATIONS_PINYIN_COUNT][32] PROGMEM = {')
for f in fulls:
    s = f[:31]
    out.append('  "%s",' % s)
out.append('};')
out.append('')
out.append('static const char kPinyinInit[STATIONS_PINYIN_COUNT][16] PROGMEM = {')
for s in inits:
    out.append('  "%s",' % s[:15])
out.append('};')
out.append('')

open(DST, 'w', encoding='utf-8').write('\n'.join(out))
print('stations: %d -> %s' % (len(entries), DST))
