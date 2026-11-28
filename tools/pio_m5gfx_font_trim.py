# CardRadio — trim M5GFX's bundled CJK fonts down to what we use.
#
# Two passes, both applied in place before M5GFX is compiled (idempotent):
#
# Pass 1 — face trim: M5GFX ships 80 CJK fonts (Chinese / Japanese / Korean /
#   Traditional, 5 sizes x 4 styles). We use exactly two of them: efontCN_14
#   (kFaceSmall) and efontCN_16 (kFaceBody). The other faces are stubbed out.
#
# Pass 2 — glyph subset: the two faces keep their FULL glyph set (~7500 CJK
#   glyphs each, 580 KB combined in flash). The UI strings are fixed, and
#   station names are known at build time, so only these need exact glyphs.
#   Dynamic text (song titles, radio metadata) falls back to GB2312 level-1:
#   the 3755 most common hanzi. Every GB2312 level-2 glyph is dropped, which
#   cuts ~270 KB — the headroom the Cardputer ADV launcher needs to fit
#   CardRadio alongside CapLab and xiaozhi on 8 MB flash. A dropped glyph
#   renders as a blank box, same as before for chars the font never had.
#
#   The .c blobs are standard u8g2 fonts: a 23-byte header, the ASCII glyph
#   table, then a unicode LUT whose entries are jump targets into one
#   continuous glyph chain (2-byte encoding + 1-byte jump + bitmap, 0x0000
#   terminator). We keep the header and ASCII table byte-for-byte, keep only
#   glyphs in the whitelist, and rebuild a segmented LUT over the new chain
#   (segment bound = last glyph encoding, per lgfx's lookup semantics).

import glob
import os
import re

from SCons.Script import DefaultEnvironment

env = DefaultEnvironment()

KEEP = {
    "lgfx_efont_cn.c": {"lgfx_efont_cn_14", "lgfx_efont_cn_16"},
    "lgfx_efont_ja.c": set(),
    "lgfx_efont_kr.c": set(),
    "lgfx_efont_tw.c": set(),
}

MARK = "/* CardRadio: trimmed */"
SUBSET_MARK = "/* CardRadio: glyph-subset */"

ARRAY_RE = re.compile(r"PROGMEM const uint8_t (\w+)\[(\d+)\]")

# ---------------------------------------------------------------- whitelist

def gb_row(row):
    out = []
    for lo in range(0xA1, 0xFF):
        try:
            out.append(bytes([row, lo]).decode("gb2312"))
        except Exception:
            pass
    return out

def build_keep_set(src_dir):
    """GB2312 punctuation row + level-1 hanzi + every non-ASCII char in src/."""
    keep = set(gb_row(0xA1))
    for row in range(0xB0, 0xD8):          # level-1: 3755 most common hanzi
        keep |= set(gb_row(row))
    if src_dir and os.path.isdir(src_dir):
        for f in (glob.glob(os.path.join(src_dir, "src", "**", "*.cpp"), recursive=True)
                  + glob.glob(os.path.join(src_dir, "src", "**", "*.h"), recursive=True)):
            try:
                with open(f, encoding="utf-8", errors="ignore") as fh:
                    for c in fh.read():
                        if ord(c) > 0x7F:
                            keep.add(c)
            except Exception:
                pass
    return keep

# ------------------------------------------------------- u8g2 blob handling

def decode_initializer(body):
    """Decode the initializer text (one "..." literal per line, last one may
    lack its closing quote) using C octal-escape semantics."""
    out = bytearray()
    for line in body.split("\n"):
        ls = line.strip()
        if not ls:
            continue
        if not ls.startswith('"'):
            continue
        s = ls[1:-1] if ls.endswith('"') else ls[1:]
        i = 0
        while i < len(s):
            c = s[i]
            if c != "\\":
                out += c.encode("utf-8", errors="surrogateescape") if ord(c) > 0x7F \
                    else c.encode("latin-1")
                i += 1
                continue
            d = ""
            j = i + 1
            while j < len(s) and len(d) < 3 and s[j] in "01234567":
                d += s[j]
                j += 1
            out.append(int(d, 8))
            i = j
    return bytes(out)

def encode_initializer(data, line_len=64):
    """Encode bytes back to C string literals (3-digit octal escapes).
    Lines are broken between tokens, never inside an escape."""
    safe = set(range(0x20, 0x7F)) - set(b'"?\\')
    lines, cur, curlen = [], [], 0
    for b in data:
        tok = chr(b) if b in safe else ("\\%03o" % b)
        if curlen + len(tok) > line_len:
            lines.append("".join(cur))
            cur, curlen = [], 0
        cur.append(tok)
        curlen += len(tok)
    if cur:
        lines.append("".join(cur))
    return '\n'.join('  "%s"' % l for l in lines)

def parse_blob(data):
    start_unicode = (data[21] << 8) | data[22]
    ascii_region = data[23 : 23 + start_unicode]
    lut_off = 23 + start_unicode
    font, p = lut_off, lut_off
    chunks, lut = [], []
    while True:
        off = (data[p] << 8) | data[p + 1]
        bound = (data[p + 2] << 8) | data[p + 3]
        font += off
        chunks.append(font)
        lut.append((off, bound))
        p += 4
        if bound >= 0xFFFF:
            break
    # one continuous glyph chain; LUT entries are jump targets into it
    glyphs = {}
    q = chunks[0]
    starts = {chunks[0]}
    while True:
        enc = (data[q] << 8) | data[q + 1]
        if enc == 0:
            break
        jump = data[q + 2]
        if jump < 3 or q + jump > len(data):
            raise ValueError("bad glyph chain at %d (enc %04X)" % (q, enc))
        glyphs[enc] = data[q : q + jump]
        starts.add(q + jump)
        q += jump
    for c in chunks[1:]:
        if c not in starts:
            raise ValueError("LUT target %d is not a glyph boundary" % c)
    return data[0:23], ascii_region, glyphs

def rebuild_blob(header, ascii_region, glyphs, seg=100):
    """Header + ASCII table verbatim, then a segmented unicode LUT, then the
    sorted glyph chain.

    lgfx picks the first LUT entry whose `bound` >= encoding and linear-scans
    the chain from that entry's target, so the LUT must be rebuilt faithfully:
    entry k points at the first glyph of segment k, and its bound is the LAST
    glyph encoding of that segment (0xFFFF on the final entry). A single
    catch-all entry (bound 0xFFFF at the chain head) forces every lookup to
    walk thousands of glyphs — that made v1.2 visually correct but ~100x
    slower to redraw text than the original font.
    """
    encs = sorted(glyphs)
    n = len(encs)
    n_seg = (n + seg - 1) // seg
    lut_size = 4 * n_seg

    chain = bytearray()
    chunks, bounds = [], []
    for k in range(n_seg):
        part = encs[k * seg : (k + 1) * seg]
        chunks.append(lut_size + len(chain))          # pos relative to LUT start
        chain += b"".join(glyphs[e] for e in part)
        bounds.append(part[-1])                       # last glyph encoding in segment

    lut = bytearray()
    prev = 0
    for k in range(n_seg):
        off = chunks[k] - prev                        # relative to previous entry
        prev = chunks[k]
        bound = bounds[k] if k < n_seg - 1 else 0xFFFF
        lut += bytes([(off >> 8) & 0xFF, off & 0xFF, (bound >> 8) & 0xFF, bound & 0xFF])

    return header + ascii_region + bytes(lut) + bytes(chain) + b"\x00\x00"

def subset_file(path, keep_encs):
    with open(path, "r", encoding="utf-8", errors="surrogateescape") as fh:
        text = fh.read()

    for match in list(ARRAY_RE.finditer(text)):
        name = match.group(1)
        if name not in ("lgfx_efont_cn_14", "lgfx_efont_cn_16"):
            continue
        m = re.search(r'PROGMEM const uint8_t %s\[(\d+)\] =\s*\n(.*?)";\n' % re.escape(name),
                      text, re.S)
        if not m:
            continue
        declared = int(m.group(1))
        data = decode_initializer(m.group(2))
        if len(data) < declared:           # C zero-fills short string initializers
            data += bytes(declared - len(data))
        try:
            header, ascii_region, glyphs = parse_blob(data)
        except Exception as exc:
            print("CardRadio: glyph subset skipped for %s (%s)" % (name, exc))
            continue
        sub = {e: g for e, g in glyphs.items() if e in keep_encs}
        new = rebuild_blob(header, ascii_region, sub)
        repl = ("PROGMEM const uint8_t %s[%d] = \n%s;\n"
                % (name, len(new), encode_initializer(new)))
        text = text[:m.start()] + repl + text[m.end():]
        print("CardRadio: %s glyph subset %d/%d glyphs, %d -> %d bytes"
              % (name, len(sub), len(glyphs), declared, len(new)))

    with open(path, "w", encoding="utf-8", errors="surrogateescape") as fh:
        fh.write(text)

# ------------------------------------------------------------------- driver

def stub(name):
    # 16 bytes, so that a stray metadata read stays inside the array.
    return "PROGMEM const uint8_t %s[16] = { 0 };\n" % name

lib_root = env.get("PROJECT_LIBDEPS_DIR") or os.path.join(
    env.get("PROJECT_DIR") or os.getcwd(), ".pio", "libdeps")
pioenv = env.get("PIOENV") or "cardputer-adv"
efont_dir = os.path.join(lib_root, pioenv, "M5GFX", "src", "lgfx", "Fonts", "efont")
src_dir = env.get("PROJECT_DIR")

keep_encs = build_keep_set(src_dir)

for filename, keep in KEEP.items():
    path = os.path.join(efont_dir, filename)
    if not os.path.isfile(path):
        continue
    try:
        with open(path, "r", encoding="utf-8", errors="surrogateescape") as fh:
            text = fh.read()

        # pass 1: stub out the faces we don't render with
        if MARK not in text:
            out, pos, changed = [], 0, False
            for match in ARRAY_RE.finditer(text):
                name = match.group(1)
                if name in keep:
                    continue
                end = text.find(";\n", match.end())     # end of this initialiser
                if end < 0:
                    continue
                out.append(text[pos:match.start()])
                out.append(stub(name))
                pos = end + 2
                changed = True
            out.append(text[pos:])
            if changed:
                text = MARK + "\n" + "".join(out)
                with open(path, "w", encoding="utf-8", errors="surrogateescape") as fh:
                    fh.write(text)
                print("CardRadio: trimmed %s" % filename)

        # pass 2: glyph subset of the kept faces
        if filename == "lgfx_efont_cn.c" and SUBSET_MARK not in text and keep_encs:
            subset_file(path, {ord(c) for c in keep_encs})
            with open(path, "r", encoding="utf-8", errors="surrogateescape") as fh:
                text = fh.read()
            with open(path, "a", encoding="utf-8", errors="surrogateescape") as fh:
                fh.write(SUBSET_MARK + "\n")
    except Exception as exc:                      # never break the build over this
        print("CardRadio: font trim skipped for %s (%s)" % (filename, exc))
