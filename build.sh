#!/bin/sh
set -eu
: "${SYSROOT:?Set SYSROOT to a Q2 glibc 2.28/NaN2008 sysroot}"
: "${GPSP:?Set GPSP to the gpSP directory with built gpsp.a}"
CC_Q2=${CC_Q2:-clang}
LD_Q2=${LD_Q2:-ld.lld}
out=${1:-q2-boy}
src=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
for unit in main platform crt-q2; do
    "$CC_Q2" --target=mipsel-linux-gnu --sysroot="$SYSROOT" \
        -isystem "$SYSROOT/usr/include/mipsel-linux-gnu" -I"$GPSP/libretro/libretro-common/include" \
        -march=mips32r2 -mabi=32 -mfp64 -mnan=2008 -mabs=2008 \
        -D_GNU_SOURCE -std=gnu11 -O2 -Wall -Wextra -c "$src/$unit.c" -o "$tmp/$unit.o"
done
"$LD_Q2" -m elf32ltsmip -e __start --dynamic-linker /lib/ld-linux-mipsn8.so.1 \
    --image-base=0x400000 -z noexecstack --hash-style=sysv --build-id=none -s \
    -o "$out" "$tmp/crt-q2.o" "$tmp/main.o" "$tmp/platform.o" "$GPSP/gpsp.a" \
    "$SYSROOT/lib/libc.so.6" "$SYSROOT/lib/libm.so.6" "$SYSROOT/lib/libdl.so.2" \
    "$SYSROOT/lib/libpthread.so.0" "$SYSROOT/lib/libasound.so.2" \
    "$SYSROOT/lib/libgcc_s.so.1" "$SYSROOT/lib/ld-linux-mipsn8.so.1"
