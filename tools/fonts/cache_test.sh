#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
out=$(mktemp "${TMPDIR:-/tmp}/meos-bizud-cache.XXXXXX")
trap 'rm -f "$out"' EXIT
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -DM5OS_BOARD_TAB5=1 \
    -Ikernel/include tools/fonts/cache_test.c kernel/lib/crc32.c -o "$out"
"$out"
