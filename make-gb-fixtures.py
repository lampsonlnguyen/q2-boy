"""Build tiny original ROMs for smoke tests. They contain no commercial game data."""
import sys
from pathlib import Path

out = Path(sys.argv[1])
out.mkdir(parents=True, exist_ok=True)
for ext, color in (("gb", False), ("gbc", True)):
    rom = bytearray(32768)
    rom[0x100:0x104] = bytes.fromhex("00 C3 50 01")  # nop; jp $0150
    rom[0x134:0x13E] = b"Q2 CORE QA"
    rom[0x143] = 0xC0 if color else 0
    rom[0x147] = 0x10 if color else 0x03  # RTC/RAM/battery on CGB, SRAM on GB
    rom[0x149] = 0x02
    rom[0x14D] = (-sum(rom[0x134:0x14D]) - 25) & 0xFF
    code = bytes.fromhex("F3 AF E0 40 3E E4 E0 47")  # di; LCD off; set DMG palette
    if color:
        code += bytes.fromhex("3E 80 E0 68 3E 1F E0 69 AF E0 69")  # CGB BG color 0 is red
    code += bytes.fromhex("3E 91 E0 40 18 FE")  # LCD on; idle
    rom[0x150:0x150 + len(code)] = code
    (out / ("core-qa." + ext)).write_bytes(rom)
