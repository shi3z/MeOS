#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
OUT=$(mktemp -d "${TMPDIR:-/tmp}/meos-peripheral.XXXXXX")
trap 'rm -rf "$OUT"' EXIT
cc -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined \
    -Ikernel/include tools/peripheral/host_test.c kernel/dev/peripheral.c -o "$OUT/test"
"$OUT/test"
