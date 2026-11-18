# Q2 NES (experimental)
Native NES Libretro frontend for DiamondBond's Q2 Pod firmware. Based on the GPL-2.0-or-later [Q2-Boy frontend](https://github.com/SlayTheEarth/q2-boy); NES core is [FCEUmm](https://github.com/libretro/libretro-fceumm) (GPL-2.0). **Not yet built or tested on actual Q2 hardware.**

## Installation layout
Extract to microSD root:
```
/.nes/q2app
/.nes/q2-nes
/.nes/fceumm_libretro.so
/q2-nes/ROMs/your-game.nes
```
The last folder is created on first run. The NES core must be compiled separately for the Q2 MIPS ABI. No ROMs are distributed.

## Build
Use the same MIPS sysroot and toolchain as upstream [Q2-Boy build guide](https://github.com/SlayTheEarth/q2-boy/blob/main/docs/BUILD.md). Obtain a compatible Libretro API header (e.g. `libretro-common/include/libretro.h`) and set `LIBRETRO_INCLUDE` to its directory. Set `SYSROOT` to the Q2 sysroot, then run `sh src/build.sh dist/.nes/q2-nes`. Package `.nes` as an SD-root ZIP.

Build `fceumm_libretro.so` separately from `libretro/libretro-fceumm` with `platform=unix`, `WANT_32BPP=0`, `HAVE_HDPACK=0` and MIPS32 NaN2008 cross compiler flags matching the sysroot. This core integration is **unverified** and may require link or ABI fixes. Do not package an x86 host .so as a Q2 core.

## Controls
Physical: Return=Up, Play/Pause=Down, Previous=Left, Next=Right, Centre=A. Touch overlay exposes B, Start, Select and menu. Configure physical and touch assignments in the frontend.

## ROM compatibility
Only .nes files are offered in the browser. FCEUmm has MMC5 support, but ROM City Rampage compatibility/performance on the Q2 has not been verified. The host currently requests RGB565 video and does not support HD texture packs or 32-bit output.

## Firmware
Requires Q2 Pod SD card games support. This app intentionally does not use PR #426's .boy-only audio bridge. Existing Q2 firmware may require updating its app discovery to recognize the new .nes launcher. In the absence of the bridge, launching with game audio takes the output as standard audio=1 games do.

## License
Frontend derived from SlayTheEarth/q2-boy, GPL-2.0-or-later. `crt-q2.c` retains original upstream Rockbox/Diamond Bond notices. The NES core is not vendored. Avoid distributing ROMs or emulator BIOS.
