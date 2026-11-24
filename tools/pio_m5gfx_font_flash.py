# CardRadio — keep M5GFX's Chinese fonts in flash instead of RAM.
#
# Why: on ESP-IDF 5 / Arduino-ESP32 3.x the PROGMEM macro is empty, so every
# `const` table ends up in .rodata — which on this chip is DRAM, not flash.
# The two Chinese faces this project uses are 21 KB + 32 KB, and they were
# sitting in RAM for the whole session, on a board that has no PSRAM at all.
#
# What: the font arrays are declared as `PROGMEM const uint8_t ...[]`. We
# replace PROGMEM with an explicit `.flash.rodata` section attribute, which
# is exactly what ICACHE_RODATA_ATTR does on ESP32 — the data stays
# CPU-readable through the memory map, it just no longer occupies RAM.
#
# The patch is applied in place, before M5GFX is compiled, and is idempotent.
# If M5GFX is ever re-downloaded (pio pkg update), the script simply patches
# the fresh copy again.

import os

from SCons.Script import DefaultEnvironment

env = DefaultEnvironment()

MARK = "CARDRADIO_FLASH_RODATA"
ATTR = '__attribute__((section(".flash.rodata")))'

lib_root = env.get("PROJECT_LIBDEPS_DIR") or os.path.join(
    env.get("PROJECT_DIR") or os.getcwd(), ".pio", "libdeps")
pioenv = env.get("PIOENV") or "cardputer-adv"

# The library may sit under a versioned folder as well.
candidates = []
base = os.path.join(lib_root, pioenv, "M5GFX")
for name in os.listdir(base) if os.path.isdir(base) else []:
    candidates.append(os.path.join(base, name, "src", "lgfx", "Fonts", "efont",
                                    "lgfx_efont_cn.c"))
candidates.append(os.path.join(base, "src", "lgfx", "Fonts", "efont",
                                "lgfx_efont_cn.c"))

for path in candidates:
    if not os.path.isfile(path):
        continue
    with open(path, "r", encoding="utf-8", errors="surrogateescape") as fh:
        text = fh.read()
    if MARK in text:
        continue
    if "PROGMEM const uint8_t" not in text:
        continue
    text = text.replace("PROGMEM const uint8_t", MARK + " const uint8_t")
    text = "#define %s %s\n%s" % (MARK, ATTR, text)
    with open(path, "w", encoding="utf-8", errors="surrogateescape") as fh:
        fh.write(text)
    print("CardRadio: M5GFX fonts moved to flash (%s)" % path)
