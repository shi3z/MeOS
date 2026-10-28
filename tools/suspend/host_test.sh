#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
OUT=$(mktemp -d "${TMPDIR:-/tmp}/meos-suspend.XXXXXX")
trap 'rm -rf "$OUT"' EXIT
cc -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined -Ikernel/include \
    tools/suspend/host_test.c kernel/lib/suspend_core.c -o "$OUT/suspend"
"$OUT/suspend"
