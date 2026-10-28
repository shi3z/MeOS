#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
OUT=$(mktemp -d "${TMPDIR:-/tmp}/meos-battery.XXXXXX")
trap 'rm -rf "$OUT"' EXIT
cc -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined -Ikernel/include \
    tools/battery/host_test.c kernel/lib/battery.c -o "$OUT/battery"
"$OUT/battery"
