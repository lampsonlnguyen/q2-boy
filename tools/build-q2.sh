#!/bin/sh
set -eu
: "${SYSROOT:?Set SYSROOT to a Q2 NaN2008 MIPS sysroot}"
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
CORE_SRC=${CORE_SRC:-"$ROOT/deps/libretro-fceumm"}
if [ ! -f "$CORE_SRC/Makefile.libretro" ]; then
  mkdir -p "$ROOT/deps"
  git clone --depth 1 https://github.com/libretro/libretro-fceumm.git "$CORE_SRC"
fi
test -f "$CORE_SRC/src/drivers/libretro/libretro-common/include/libretro.h" || {
  echo "FCEUmm Libretro headers missing" >&2; exit 1;
}
export LIBRETRO_INCLUDE="$CORE_SRC/src/drivers/libretro/libretro-common/include"
Q2_CC="clang --target=mipsel-linux-gnu --sysroot=$SYSROOT -march=mips32r2 -mabi=32 -mfp64 -mnan=2008 -mabs=2008 -fuse-ld=lld"
make -C "$CORE_SRC" -f Makefile.libretro -j"${JOBS:-2}" \
  platform=unix WANT_32BPP=0 HAVE_HDPACK=0 HAVE_NTSC=0 \
  CC="$Q2_CC" PLATFORM_DEFINES="-O2 -fPIC -D_GNU_SOURCE" \
  LDFLAGS="--sysroot=$SYSROOT -fuse-ld=lld -Wl,--hash-style=sysv"
STAGE="$ROOT/dist/card"
mkdir -p "$STAGE/.nes"
cp "$ROOT/package/.nes/q2app" "$STAGE/.nes/q2app"
cp "$CORE_SRC/fceumm_libretro.so" "$STAGE/.nes/fceumm_libretro.so"
sh "$ROOT/src/build.sh" "$STAGE/.nes/q2-nes"
python3 "$ROOT/tools/verify-package.py" "$STAGE/.nes"
(cd "$STAGE" && zip -q -r "$ROOT/dist/q2-nes.zip" .nes)
echo "Created $ROOT/dist/q2-nes.zip"
