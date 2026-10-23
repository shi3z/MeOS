#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
OUT=$(mktemp -d "${TMPDIR:-/tmp}/meos-touch.XXXXXX")
trap 'rm -rf "$OUT"' EXIT
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined \
    -DM5OS_BOARD_TAB5=1 -Ikernel/include \
    tools/touch/host_test.c drivers/tab5kbd/tab5touch.c -o "$OUT/touch_test"
"$OUT/touch_test"
