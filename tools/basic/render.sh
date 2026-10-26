#!/bin/sh
set -eu
# Arguments resolve against the caller's working directory. Run from repo root
# for examples, e.g. sh tools/basic/render.sh user/basic/examples/sine_plot.bas /tmp/sine.svg
ROOT=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
OUT=$(mktemp -d "${TMPDIR:-/tmp}/meos-basic-render.XXXXXX")
trap 'rm -rf "$OUT"' EXIT
cc -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined,float-cast-overflow \
    -DM5OS_BOARD_TAB5=1 -I"$ROOT/kernel/include" \
    "$ROOT/tools/basic/render.c" "$ROOT/user/basic/basic.c" "$ROOT/user/basic/graphics.c" \
    "$ROOT/third_party/ubasic/ubasic.c" "$ROOT/third_party/ubasic/tokenizer.c" \
    "$ROOT/drivers/lcd/display_claim.c" "$ROOT/kernel/lib/number.c" "$ROOT/kernel/lib/number_math.c" \
    -o "$OUT/render" -lm
"$OUT/render" "$@"
