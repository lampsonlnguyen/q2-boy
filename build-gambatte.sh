#!/bin/sh
set -eu
: "${GAMBATTE:?Set GAMBATTE to the Gambatte source checkout}"
: "${SYSROOT:?Set SYSROOT to the Q2 sysroot}"
: "${CXX_HEADERS:?Set CXX_HEADERS to the MIPS Debian libstdc++ 10 header package root}"
CC_Q2=${CC_Q2:-clang}
LD_Q2=${LD_Q2:-ld.lld}
out=${1:-gambatte_libretro.so}
target="--target=mipsel-linux-gnu --sysroot=$SYSROOT -isystem $SYSROOT/usr/include/mipsel-linux-gnu -march=mips32r2 -mabi=32 -mfp64 -mnan=2008 -mabs=2008"
headers="-nostdinc++ -isystem $CXX_HEADERS/usr/include/c++/10 -isystem $CXX_HEADERS/usr/include/mipsel-linux-gnu/c++/10"
out=$(realpath -m "$out")
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
# The firmware sysroot has no shared-library CRT objects. Finalize C++ globals
# while their code is still mapped when the frontend calls dlclose.
"$CC_Q2" $target -xc -fPIC -c - -o "$tmp/dso.o" <<'EOF'
extern void __cxa_finalize(void *);
__attribute__((visibility("hidden"))) void *__dso_handle = &__dso_handle;
__attribute__((destructor)) static void finalize_cpp(void) {
    __cxa_finalize(__dso_handle);
}
EOF
libs="$tmp/dso.o $SYSROOT/lib/libstdc++.so.6 $SYSROOT/lib/libgcc_s.so.1 $SYSROOT/lib/libc.so.6 $SYSROOT/lib/libm.so.6 $SYSROOT/lib/ld-linux-mipsn8.so.1"
rm -f "$out"
make -C "$GAMBATTE" -j "${JOBS:-4}" platform=unix HAVE_NETWORK=0 VIDEO_RGB565=1 \
    STATIC_LINKING=0 TARGET="$out" CC="$CC_Q2 $target" CXX="$CC_Q2 $target $headers" \
    LD="$CC_Q2 $target -nostdlib -fuse-ld=$LD_Q2" \
    LDFLAGS="-Wl,--no-undefined,--hash-style=sysv,-z,noexecstack,-s $libs"
