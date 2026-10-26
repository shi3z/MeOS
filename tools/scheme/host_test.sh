#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
scheme_test_dir=$(mktemp -d "${TMPDIR:-/tmp}/meos-scheme.XXXXXX")
trap 'rm -rf "$scheme_test_dir"' EXIT
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -g -Ikernel/include -fsanitize=address,undefined \
    tools/scheme/host_test.c user/scheme/scheme.c user/scheme/buffer.c \
    kernel/lib/number.c -o "$scheme_test_dir/test" -lm
"$scheme_test_dir/test"
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -g -Ikernel/include -DM5OS_BOARD_TAB5=1 \
    -fsanitize=address,undefined tools/scheme/ide_test.c fs/vfs/write_file.c user/scheme/scheme.c \
    user/scheme/buffer.c kernel/lib/number.c \
    -o "$scheme_test_dir/ide-test" -lm
"$scheme_test_dir/ide-test"
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -DM5OS_BOARD_TAB5=1 -Ikernel/include \
    -fsyntax-only user/scheme/ide.c
