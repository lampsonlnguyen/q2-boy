#!/usr/bin/env python3
from pathlib import Path
import sys

def check(path):
    h = path.read_bytes()[:20]
    if len(h) < 20 or h[:4] != b"\x7fELF":
        raise ValueError(f"{path}: invalid ELF")
    if h[4] != 1 or h[5] != 1 or int.from_bytes(h[18:20], "little") != 8:
        raise ValueError(f"{path}: expected MIPS32 little-endian ELF")

directory = Path(sys.argv[1])
manifest = (directory / "q2app").read_text()
if "game=1" not in manifest or "audio=1" not in manifest:
    raise ValueError("Invalid card manifest")
for name in ("q2-nes", "fceumm_libretro.so"):
    check(directory / name)
print("MIPS binaries and manifest validated")
