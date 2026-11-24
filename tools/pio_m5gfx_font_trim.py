# CardRadio — trim M5GFX's bundled CJK font files down to what we use.
#
# Why: M5GFX ships 80 CJK fonts (Chinese / Japanese / Korean / Traditional,
# 5 sizes x 4 styles). We use exactly two of them: efontCN_16 for names and
# titles, efontCN_12 for the small chrome. The four source files behind them
# add up to 94 MB of string literals, and the compiler has to read every byte
# of them on each build — which is where most of the build time went.
#
# The fonts live in flash, not RAM, so this does not change the memory
# budget; it makes builds fast again. The linker already dropped the unused
# faces from the firmware, so nothing there changes either.
#
# The patch is applied in place before M5GFX is compiled, and is idempotent.
# If M5GFX is ever re-downloaded, the script simply re-trims it.

import os
import re

from SCons.Script import DefaultEnvironment

env = DefaultEnvironment()

# The two faces the UI actually renders with (src/main.cpp: kFaceBody /
# kFaceSmall). English text uses M5GFX's built-in DejaVu faces, which live
# in other files and are not touched. Japanese, Korean and Traditional
# Chinese are not shown at all.
KEEP = {
    "lgfx_efont_cn.c": {"lgfx_efont_cn_12", "lgfx_efont_cn_14", "lgfx_efont_cn_16"},
    "lgfx_efont_ja.c": set(),
    "lgfx_efont_kr.c": set(),
    "lgfx_efont_tw.c": set(),
}

MARK = "/* CardRadio: trimmed */"

ARRAY_RE = re.compile(r"PROGMEM const uint8_t (\w+)\[(\d+)\]")


def stub(name):
    # 16 bytes, so that a stray metadata read stays inside the array.
    return "PROGMEM const uint8_t %s[16] = { 0 };\n" % name


lib_root = env.get("PROJECT_LIBDEPS_DIR") or os.path.join(
    env.get("PROJECT_DIR") or os.getcwd(), ".pio", "libdeps")
pioenv = env.get("PIOENV") or "cardputer-adv"
efont_dir = os.path.join(lib_root, pioenv, "M5GFX", "src", "lgfx", "Fonts", "efont")

for filename, keep in KEEP.items():
    path = os.path.join(efont_dir, filename)
    if not os.path.isfile(path):
        continue
    try:
        with open(path, "r", encoding="utf-8", errors="surrogateescape") as fh:
            text = fh.read()
        if MARK in text:
            continue

        out = []
        pos = 0
        changed = False
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

        if not changed:
            continue
        with open(path, "w", encoding="utf-8", errors="surrogateescape") as fh:
            fh.write(MARK + "\n" + "".join(out))
        print("CardRadio: trimmed %s" % filename)
    except Exception as exc:                      # never break the build over this
        print("CardRadio: font trim skipped for %s (%s)" % (filename, exc))
