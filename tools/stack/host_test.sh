#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
OUT=$(mktemp -d "${TMPDIR:-/tmp}/meos-stack.XXXXXX")
trap 'rm -rf "$OUT"' EXIT
for ARCH in esp32p4 esp32s3; do
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined \
        -Ikernel/include tools/stack/host_test.c kernel/task/task.c kernel/task/stack.c \
        hal/$ARCH/context_init.c -o "$OUT/$ARCH"
    "$OUT/$ARCH"
done
