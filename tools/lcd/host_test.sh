#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
OUT=$(mktemp -d "${TMPDIR:-/tmp}/meos-lcd.XXXXXX")
trap 'rm -rf "$OUT"' EXIT
cc -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined \
    -DM5OS_BOARD_TAB5=1 -Ikernel/include tools/lcd/resize_test.c -o "$OUT/resize"
"$OUT/resize"
