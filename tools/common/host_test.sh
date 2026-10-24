#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
OUT=$(mktemp -d "${TMPDIR:-/tmp}/meos-common.XXXXXX")
trap 'rm -rf "$OUT"' EXIT
cc -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined,float-cast-overflow -Ikernel/include tools/common/number_test.c kernel/lib/number.c kernel/lib/number_math.c -lm -o "$OUT/test"
"$OUT/test"
sh tools/calc/host_test.sh
