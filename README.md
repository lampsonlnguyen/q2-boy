# Q2 NES (experimental)

A native Q2 Pod NES Libretro frontend adapted from [SlayTheEarth/q2-boy](https://github.com/SlayTheEarth/q2-boy) (GPL-2.0-or-later), using [FCEUmm](https://github.com/libretro/libretro-fceumm) (GPL-2.0).

**Not yet built for or tested on physical Q2 hardware.** The Pocket Boy-specific firmware audio bridge in PR #426 does not apply to this app.

## Cross-compile and package

Use Linux or WSL with clang, lld, git, make, zip and python3 and the Shanling Q2 glibc 2.28 / NaN2008 MIPS sysroot with ALSA headers. For sysroot instructions see [Q2-Boy BUILD.md](https://github.com/SlayTheEarth/q2-boy/blob/main/docs/BUILD.md).

```sh
export SYSROOT=/absolute/path/to/q2-sysroot
sh tools/build-q2.sh
```

The script clones FCEUmm into ignored deps/ (or use CORE_SRC to point to an existing checkout), builds a little-endian MIPS32 RGB565 core without HD packs or NTSC filters, builds the Q2 frontend and checks ELF headers. It generates dist/q2-nes.zip **only if compilation succeeds**.

## Installation

Extract the ZIP at the SD card root, creating:

```text
/.nes/q2app
/.nes/q2-nes
/.nes/fceumm_libretro.so
/q2-nes/ROMs/your-file.nes
```

The ROMs folder is created at first launch. Select Library → Games → Q2 NES. No ROMs or BIOS files included.

## Controls and compatibility

Return=Up, Play/Pause=Down, Previous=Left, Next=Right, Centre=A. The touchscreen offers B, Start, Select and Menu. Saves stored in /q2-nes/Saves.

NES source frames up to 256×240 are scaled to 375×320. FCEUmm implements MMC5, required by ROM City Rampage, but compatibility and speed on Q2 are unverified. Inspect ROM header via python3 tools/check-rom.py /path/to/game.nes.
