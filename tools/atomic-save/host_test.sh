#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
OUT=$(mktemp -d "${TMPDIR:-/tmp}/meos-atomic-save.XXXXXX")
trap 'rm -rf "$OUT"' EXIT
${CC:-cc} -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined \
    -DM5OS_BOARD_TAB5=1 -Ikernel/include tools/atomic-save/vfs_test.c \
    fs/vfs/vfs.c fs/vfs/write_file.c -o "$OUT/vfs"
"$OUT/vfs"
