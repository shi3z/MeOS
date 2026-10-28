#!/bin/sh
# Builds and runs the input method's host-side tests (no board needed):
#   tools/ime/host_test.sh [dict.bin]     dict.bin: optional large dictionary from tools/ime_mkdict.py
set -e
cd "$(dirname "$0")/../.."
OUT=${TMPDIR:-/tmp}/ime_host_test
mkdir -p "$OUT"
CFLAGS="-Wall -Wextra -g -O1 -DM5OS_BOARD_TAB5=1"
SRC=drivers/ime
cc $CFLAGS -o "$OUT/romaji_test" tools/ime/romaji_test.c $SRC/romaji.c
"$OUT/romaji_test"
cc $CFLAGS -Ikernel/include -o "$OUT/ime_test" tools/ime/host_ime_test.c $SRC/ime.c $SRC/conv.c $SRC/romaji.c $SRC/ime_dict.c $SRC/ime_mini_dict.c
"$OUT/ime_test" "$@"
