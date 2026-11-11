#!/bin/sh
# File-backed tests for mtool.  No root and no /dev/mem needed.
set -eu

MTOOL=${1:-./mtool}
TMPDIR_TEST=$(mktemp -d)
trap 'rm -rf "$TMPDIR_TEST"' EXIT

F="$TMPDIR_TEST/mem.bin"
dd if=/dev/zero of="$F" bs=4096 count=1 status=none

fail() {
    echo "FAIL: $*" >&2
    exit 1
}

check_contains() {
    haystack=$1
    needle=$2
    label=$3
    case "$haystack" in
        *"$needle"*) ;;
        *) fail "$label: expected '$needle' in: $haystack" ;;
    esac
}

check_not_contains() {
    haystack=$1
    needle=$2
    label=$3
    case "$haystack" in
        *"$needle"*) fail "$label: did not expect '$needle' in: $haystack" ;;
    esac
}

# 32-bit write / read
"$MTOOL" w.l --file "$F" 0x10 0x11223344
out=$("$MTOOL" r.l --file "$F" 0x10 1)
check_contains "$out" "11223344" "r.l"

# 64-bit
"$MTOOL" w.q --file "$F" 0x20 0x1122334455667788
out=$("$MTOOL" r.q --file "$F" 0x20 1)
check_contains "$out" "1122334455667788" "r.q"

# 16-bit
"$MTOOL" w.w --file "$F" 0x30 0xabcd
out=$("$MTOOL" r.w --file "$F" 0x30 1)
check_contains "$out" "abcd" "r.w"

# 8-bit
"$MTOOL" w.b --file "$F" 0x40 0x5a
out=$("$MTOOL" r.b --file "$F" 0x40 1)
check_contains "$out" "5a" "r.b"

# Multiple values, little-endian layout
"$MTOOL" w.l --file "$F" 0x50 0x01020304 0x05060708
out=$("$MTOOL" r.l --file "$F" 0x50 0x8)
check_contains "$out" "01020304 05060708" "multiple values"

# Reading from inside a word returns the whole word, based at its boundary.
"$MTOOL" w.l --file "$F" 0x60 0xdeadbeef
out=$("$MTOOL" r.l --file "$F" 0x61 0x1)
check_contains "$out" "0000000000000060: deadbeef" "start rounding"

# LEN is a byte count, and .l rounds up to whole 32-bit words.
"$MTOOL" w.l --file "$F" 0x90 0x11111111 0x22222222
out=$("$MTOOL" r.l --file "$F" 0x90 0x1)
check_contains "$out" "11111111" "len=1 returns one word"
check_not_contains "$out" "22222222" "len=1 overread"
out=$("$MTOOL" r.l --file "$F" 0x90 0x4)
check_contains "$out" "11111111" "len=4 returns one word"
check_not_contains "$out" "22222222" "len=4 overread"
out=$("$MTOOL" r.l --file "$F" 0x90 0x5)
check_contains "$out" "11111111 22222222" "len=5 spans two words"

# ASCII column and --no-ascii
out=$("$MTOOL" r.b --file "$F" 0x40 1)
check_contains "$out" "Z" "ascii column"
out=$("$MTOOL" r.b --file "$F" 0x40 1 --no-ascii)
case "$out" in
    *Z*) fail "--no-ascii still printed the ASCII column" ;;
esac

# Write confirmation only with -v
out=$("$MTOOL" w.l --file "$F" 0x70 0x12345678)
[ -z "$out" ] || fail "silent write printed: $out"
out=$("$MTOOL" w.l --file "$F" 0x70 0x12345678 -v)
check_contains "$out" "12345678" "verbose write"

# Out-of-range must fail
if "$MTOOL" r.l --file "$F" 0x1000 1 >/dev/null 2>&1; then
    fail "out-of-range read succeeded"
fi

# --read-only cannot write
if "$MTOOL" w.l --file "$F" 0x10 1 --read-only >/dev/null 2>&1; then
    fail "--read-only write succeeded"
fi

# Numbers are hex by default; --decimal opts out.
"$MTOOL" w.l --file "$F" 128 0xa5a5a5a5 --decimal
out=$("$MTOOL" r.l --file "$F" 0x80 1)
check_contains "$out" "a5a5a5a5" "decimal address"

# PCI listing is read-only.  It is opt-in because a minimal CI container
# may expose an incomplete or permission-restricted /sys tree.
if [ "${MTOOL_TEST_PCI:-0}" = "1" ] && [ -d /sys/bus/pci/devices ]; then
    "$MTOOL" pci >/dev/null || fail "pci listing failed"
fi

echo "all mtool file-backed tests passed"