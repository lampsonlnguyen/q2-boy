#!/usr/bin/env python3
from pathlib import Path
import sys

path = Path(sys.argv[1])
h = path.read_bytes()[:16]
if len(h) < 16 or h[:4] != b"NES\x1a":
    raise ValueError("Not an iNES/NES 2.0 file")
nes2 = (h[7] & 12) == 8
mapper = (h[6] >> 4) | (h[7] & 0xf0)
if nes2:
    mapper |= (h[8] & 15) << 8
print(f"{path.name}: {'NES 2.0' if nes2 else 'iNES'} mapper={mapper}, bytes={path.stat().st_size}")
