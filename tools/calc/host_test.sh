#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
OUT=$(mktemp -d "${TMPDIR:-/tmp}/meos-calc.XXXXXX")
trap 'rm -rf "$OUT"' EXIT
cc -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined,float-cast-overflow \
    tools/calc/host_test.c apps/calculator/expr.c kernel/lib/number.c kernel/lib/number_math.c -Ikernel/include -lm -o "$OUT/test"
"$OUT/test"
