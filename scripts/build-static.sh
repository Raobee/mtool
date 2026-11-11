#!/bin/sh
# Build statically linked mtool binaries.
#
# Usage:
#   sh scripts/build-static.sh [all|x86_64|aarch64|armv7]
#
# Compiler overrides (useful for a local toolchain):
#   CC_X86_64=... CC_AARCH64=... CC_ARM32=...
#
# The x86_64 build uses musl-gcc, so the result does not depend on the
# target's glibc at all.  The ARM builds use the usual Debian/Ubuntu
# cross toolchains with -static; the resulting ELF contains libc.
set -eu

ARCH=${1:-all}
OUT=${OUT:-dist}

CC_X86_64=${CC_X86_64:-musl-gcc}
CC_AARCH64=${CC_AARCH64:-aarch64-linux-gnu-gcc}
CC_ARM32=${CC_ARM32:-arm-linux-gnueabihf-gcc}

CFLAGS=${CFLAGS:--O2 -std=c99 -Wall -Wextra -Wpedantic -fno-strict-aliasing}
CPPFLAGS=${CPPFLAGS:--D_GNU_SOURCE -D_FILE_OFFSET_BITS=64}

mkdir -p "$OUT"

cc_for() {
    case "$1" in
    x86_64)  echo "$CC_X86_64" ;;
    aarch64) echo "$CC_AARCH64" ;;
    armv7)   echo "$CC_ARM32" ;;
    *)
        echo "unknown arch: $1" >&2
        return 1
        ;;
    esac
}

build_one() {
    arch=$1
    cc=$(cc_for "$arch") || exit 1
    out="$OUT/mtool.$arch"

    if ! command -v "$cc" >/dev/null 2>&1; then
        echo "error: compiler '$cc' for $arch not found" >&2
        exit 1
    fi

    echo "==> $arch: $cc"
    # shellcheck disable=SC2086
    "$cc" $CPPFLAGS $CFLAGS -static -o "$out" mtool.c

    if command -v file >/dev/null 2>&1; then
        file "$out"
    fi
    if command -v readelf >/dev/null 2>&1; then
        if readelf -d "$out" 2>/dev/null | grep -q 'NEEDED'; then
            echo "error: $out is dynamically linked" >&2
            exit 1
        fi
    fi
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$out" > "$out.sha256"
    fi
}

case "$ARCH" in
all)
    build_one x86_64
    build_one aarch64
    build_one armv7
    ;;
x86_64 | aarch64 | armv7)
    build_one "$ARCH"
    ;;
*)
    echo "usage: $0 [all|x86_64|aarch64|armv7]" >&2
    exit 1
    ;;
esac