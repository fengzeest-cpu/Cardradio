# Puts the Xtensa toolchain on PATH.
#
# pioarduino 55.x ships the compiler as a `tools.json` descriptor instead of a
# normal package, and PlatformIO Core 6.2 does not download what it describes.
# On this machine the toolchain was unpacked by hand, so point the build at it.
# If the compiler is already on PATH this does nothing.
import os
from pathlib import Path

Import("env")

core = Path(os.environ.get("PLATFORMIO_CORE_DIR") or (Path.home() / ".platformio"))
roots = [
    core / "packages" / "toolchain-xtensa-esp-elf" / "xtensa-esp-elf" / "bin",
    core / "packages" / "toolchain-xtensa-esp-elf" / "bin",
]
for root in roots:
    if (root / "xtensa-esp32s3-elf-g++").exists() or (root / "xtensa-esp32s3-elf-g++.exe").exists():
        env.PrependENVPath("PATH", str(root))
        print("Using Xtensa toolchain from %s" % root)
        break
